// file      : bpkg/fetch-git.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch.hxx>

#include <map>
#include <algorithm> // find(), find_if(), replace(), sort()

#include <libbutl/git.mxx>
#include <libbutl/utility.mxx>          // digit(), xdigit()
#include <libbutl/process.mxx>
#include <libbutl/filesystem.mxx>       // path_match()
#include <libbutl/semantic-version.mxx>
#include <libbutl/standard-version.mxx> // parse_standard_version()

#include <bpkg/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  struct fail_git
  {
    [[noreturn]] void
    operator() (const diag_record& r) const
    {
      if (verb < 2)
        r << info << "re-run with -v for more information";

      r << endf;
    }
  };

  static const diag_noreturn_end<fail_git> endg;

  using opt = optional<const char*>; // Program option.

  static strings
  timeout_opts (const common_options& co, repository_protocol proto)
  {
    if (!co.fetch_timeout_specified ())
      return strings ();

    switch (proto)
    {
    case repository_protocol::http:
    case repository_protocol::https:
      {
        // Git doesn't support the connection timeout option. The options we
        // use instead are just an approximation of the former, that, in
        // particular, doesn't cover the connection establishing. Sensing
        // HTTP(s) smart vs dumb protocol using a fetch utility prior to
        // running git (see below) will probably mitigate this somewhat.
        //
        return strings ({
            "-c", "http.lowSpeedLimit=1",
            "-c", "http.lowSpeedTime=" + to_string (co.fetch_timeout ())});
      }
    case repository_protocol::git:
      {
        warn << "--fetch-timeout is not supported by the git protocol";
        break;
      }
    case repository_protocol::ssh:
      {
        // The way to support timeout for the ssh protocol would be using the
        // '-c core.sshCommand=...' git option (relying on ConnectTimeout and
        // ServerAlive* options for OpenSSH). To do it cleanly, we would need
        // to determine the ssh program path and kind (ssh, putty, plink, etc)
        // that git will use to communicate with the repository server. And it
        // looks like there is no easy way to do it (see the core.sshCommand
        // and ssh.variant git configuration options for details). So we will
        // not support the ssh protocol timeout for now. Note that the user
        // can always specify the timeout in git or ssh configuration.
        //
        warn << "--fetch-timeout is not supported by the ssh protocol";
        break;
      }
    case repository_protocol::file: return strings (); // Local communications.
    }

    assert (false); // Can't be here.
    return strings ();
  }

  template <typename... A>
  static string
  git_line (const common_options&, const char* what, A&&... args);

  // Start git process. On the first call check that git version is 2.11.0 or
  // above, and fail if that's not the case. Note that the full functionality
  // (such as being able to fetch unadvertised commits) requires 2.14.0. And
  // supporting versions prior to 2.11.0 doesn't seem worth it (plus other
  // parts of the toolchain also requires 2.11.0).
  //
  // Also note that git is executed in the "sanitized" environment, having the
  // environment variables that are local to the repository being unset (all
  // except GIT_CONFIG_PARAMETERS). We do the same as the git-submodule script
  // does for commands executed for submodules. Though we do it for all
  // commands (including the ones related to the top repository).
  //
  static semantic_version git_ver;
  static optional<strings> unset_vars;

  template <typename O, typename E, typename... A>
  static process
  start_git (const common_options& co,
             O&& out,
             E&& err,
             A&&... args)
  {
    try
    {
      // Prior the first git run check that its version is fresh enough and
      // setup the sanitized environment.
      //
      if (!unset_vars)
      {
        unset_vars = strings ();

        for (;;) // Breakout loop.
        {
          // Check git version.
          //
          // We assume that non-sanitized git environment can't harm this call.
          //
          string s (git_line (co, "git version",
                              co.git_option (),
                              "--version"));

          optional<semantic_version> v (git_version (s));

          if (!v)
            fail << "'" << s << "' doesn't appear to contain a git version" <<
              info << "produced by '" << co.git () << "'; "
                 << "use --git to override" << endg;

          if (*v < semantic_version {2, 11, 0})
            fail << "unsupported git version " << *v <<
              info << "minimum supported version is 2.11.0" << endf;

          git_ver = move (*v);

          // Sanitize the environment.
          //
          fdpipe pipe (open_pipe ());

          // We assume that non-sanitized git environment can't harm this call.
          //
          process pr (start_git (co,
                                 pipe, 2 /* stderr */,
                                 co.git_option (),
                                 "rev-parse",
                                 "--local-env-vars"));

          // Shouldn't throw, unless something is severely damaged.
          //
          pipe.out.close ();

          try
          {
            ifdstream is (move (pipe.in),
                          fdstream_mode::skip,
                          ifdstream::badbit);

            for (string l; !eof (getline (is, l)); )
            {
              if (l != "GIT_CONFIG_PARAMETERS")
                unset_vars->push_back (move (l));
            }

            is.close ();

            if (pr.wait ())
              break;

            // Fall through.
          }
          catch (const io_error&)
          {
            if (pr.wait ())
              fail << "unable to read git local environment variables" << endg;

            // Fall through.
          }

          // We should only get here if the child exited with an error status.
          //
          assert (!pr.wait ());

          fail << "unable to list git local environment variables" << endg;
        }
      }

      return process_start_callback ([] (const char* const args[], size_t n)
                                     {
                                       if (verb >= 2)
                                         print_process (args, n);
                                     },
                                     0 /* stdin */, out, err,
                                     process_env (co.git (), *unset_vars),
                                     forward<A> (args)...);
    }
    catch (const process_error& e)
    {
      fail << "unable to execute " << co.git () << ": " << e << endg;
    }
  }

  // Run git process.
  //
  template <typename... A>
  static process_exit
  run_git (const common_options& co, A&&... args)
  {
    process pr (start_git (co,
                           1 /* stdout */, 2 /* stderr */,
                           forward<A> (args)...));
    pr.wait ();
    return *pr.exit;
  }

  // Run git process and return it's output as a string. Fail if the output
  // doesn't contain a single line.
  //
  template <typename... A>
  static string
  git_line (const common_options& co, const char* what, A&&... args)
  {
    fdpipe pipe (open_pipe ());
    process pr (start_git (co, pipe, 2 /* stderr */, forward<A> (args)...));
    pipe.out.close (); // Shouldn't throw, unless something is severely damaged.

    try
    {
      ifdstream is (move (pipe.in), fdstream_mode::skip);

      optional<string> r;
      if (is.peek () != ifdstream::traits_type::eof ())
      {
        string s;
        getline (is, s);

        if (!is.eof () && is.peek () == ifdstream::traits_type::eof ())
          r = move (s);
      }

      is.close ();

      if (pr.wait ())
      {
        if (r)
          return *r;

        fail << "invalid " << what << endg;
      }

      // Fall through.
    }
    catch (const io_error&)
    {
      if (pr.wait ())
        fail << "unable to read " << what << endg;

      // Fall through.
    }

    // We should only get here if the child exited with an error status.
    //
    assert (!pr.wait ());

    fail << "unable to obtain " << what << endg;
  }

  // Convert the URL object to string representation that is usable in the git
  // commands. This, in particular, means using file:// (rather than local
  // path) notation for local URLs.
  //
  // Note that cloning the local git repository using the local path notation
  // disregards --depth option (and issues a warning), creating full copy of
  // the source repository (copying some files and hard-linking others if
  // possible). Using --no-local option overrides such an unwanted behavior.
  // However, this options can not be propagated to submodule--helper's clone
  // command that we use to clone submodules. So to truncate local submodule
  // histories we will use the file URL notation for local repositories.
  //
  static string
  to_git_url (const repository_url& url)
  {
    if (url.scheme != repository_protocol::file)
      return url.string ();

#ifndef _WIN32
    // Enforce the 'file://' notation for local URLs (see libpkg/manifest.hxx).
    //
    repository_url u (url.scheme,
                      repository_url::authority_type (),
                      url.path,
                      url.query);

    return u.string ();
#else
    // On Windows the appropriate file notations are:
    //
    // file://c:/...
    // file://c:\...
    //
    // Note that none of them conforms to RFC3986. The proper one should be:
    //
    // file:///c:/...
    //
    // We choose to convert it to the "most conformant" (the first)
    // representation to ease the fix-up before creating the URL object from
    // it, when required.
    //
    string p (url.path->string ());
    replace (p.begin (), p.end (), '\\', '/');
    return "file://" + p;
#endif
  }

  // Create the URL object from a string representation printed by git
  // commands.
  //
  static repository_url
  from_git_url (string&& u)
  {
    // Fix-up the broken Windows file URL notation (see to_git_url() for
    // details).
    //
#ifdef _WIN32
    if (casecmp (u, "file://", 7) == 0 && u[7] != '/')
      u.insert (7, 1, '/');
#endif

    repository_url r (u);

    path& up (*r.path);

    if (!up.to_directory ())
      up = path_cast<dir_path> (move (up));

    return r;
  }

  // Get/set the repository configuration option.
  //
  inline static string
  config_get (const common_options& co,
              const dir_path& dir,
              const string& key,
              const char* what)
  {
    return git_line (co,
                     what,
                     co.git_option (),
                     "-C", dir,
                     "config",
                     "--get",
                     key);
  }

  inline static void
  config_set (const common_options& co,
              const dir_path& dir,
              const string& key,
              const string& value)
  {
    run_git (co, co.git_option (), "-C", dir, "config", key, value);
  }

  // Get option from the specified configuration file.
  //
  inline static string
  config_get (const common_options& co,
              const path& file,
              const string& key,
              const char* what)
  {
    return git_line (co,
                     what,
                     co.git_option (),
                     "config",
                     "--file", file,
                     "--get",
                     key);
  }

  // Get/set the repository remote URL.
  //
  static repository_url
  origin_url (const common_options& co, const dir_path& dir)
  {
    try
    {
      return from_git_url (
        config_get (co, dir, "remote.origin.url", "repository remote URL"));
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid remote.origin.url configuration value: " << e << endg;
    }
  }

  inline static void
  origin_url (const common_options& co,
              const dir_path& dir,
              const repository_url& url)
  {
    config_set (co, dir, "remote.origin.url", to_git_url (url));
  }

  // Sense the git protocol capabilities for a specified URL.
  //
  // Protocols other than HTTP(S) are considered smart but without the
  // unadvertised refs (note that this is a pessimistic assumption for
  // git:// and ssh://).
  //
  // For HTTP(S) sense the protocol type by sending the first HTTP request of
  // the fetch operation handshake and analyzing the first line of the
  // response. Fail if connecting to the server failed, the response code
  // differs from 200, or reading the response body failed.
  //
  // Note that, as a side-effect, this function checks the HTTP(S) server
  // availability and so must be called prior to any git command that involves
  // communication to the remote server. Not doing so may result in the command
  // hanging indefinitely while trying to establish TCP/IP connection (see the
  // timeout_opts() function for the gory details).
  //
  enum class capabilities
  {
    dumb,  // No shallow clone support.
    smart, // Support for shallow clone, but not for unadvertised refs fetch.
    unadv  // Support for shallow clone and for unadvertised refs fetch.
  };

  static capabilities
  sense_capabilities (const common_options& co, repository_url url)
  {
    assert (url.path);

    switch (url.scheme)
    {
    case repository_protocol::git:
    case repository_protocol::ssh:
    case repository_protocol::file: return capabilities::smart;
    case repository_protocol::http:
    case repository_protocol::https: break; // Ask the server (see below).
    }

    path& up (*url.path);

    if (!up.to_directory ())
      up = path_cast<dir_path> (move (up));

    up /= path ("info/refs");

    if (url.query)
      *url.query += "&service=git-upload-pack";
    else
      url.query = "service=git-upload-pack";

    string u (url.string ());
    process pr (start_fetch (co, u));

    try
    {
      // We unset failbit to properly handle an empty response (no refs) from
      // the dumb server.
      //
      ifdstream is (move (pr.in_ofd),
                    fdstream_mode::skip | fdstream_mode::binary,
                    ifdstream::badbit);

      string l;
      getline (is, l); // Is empty if no refs returned by the dumb server.

      // If the first response line has the following form:
      //
      // XXXX# service=git-upload-pack"
      //
      // where XXXX is a sequence of 4 hex digits, then the server implements
      // the smart protocol.
      //
      // Note that to consider the server to be "smart" it would make sense
      // to also check that the response Content-Type header value is
      // 'application/x-git-upload-pack-advertisement'. However, we will skip
      // this check in order to not complicate the fetch API.
      //
      size_t n (l.size ());

      capabilities r (
        n >= 4 &&
        xdigit (l[0]) && xdigit (l[1]) && xdigit (l[2]) && xdigit (l[3]) &&
        l.compare (4, n - 4, "# service=git-upload-pack") == 0
        ? capabilities::smart
        : capabilities::dumb);

      // If the transport is smart let's see it the server also supports
      // unadvertised refs fetch.
      //
      if (r == capabilities::smart && !is.eof ())
      {
        getline (is, l);

        // Parse the space-separated list of capabilities that follows the
        // NULL character.
        //
        for (size_t p (l.find ('\0')); p != string::npos; )
        {
          size_t e (l.find (' ', ++p));
          size_t n (e != string::npos ? e - p : e);

          if (l.compare (p, n, "allow-reachable-sha1-in-want") == 0 ||
              l.compare (p, n, "allow-tip-sha1-in-want") == 0)
          {
            r = capabilities::unadv;
            break;
          }

          p = e;
        }
      }

      is.close ();

      if (pr.wait ())
        return r;

      // Fall through.
    }
    catch (const io_error&)
    {
      if (pr.wait ())
        fail << "unable to read fetched " << url << endg;

      // Fall through.
    }

    // We should only get here if the child exited with an error status.
    //
    assert (!pr.wait ());

    fail << "unable to fetch " << url << endg;
  }

  // A git ref (tag, branch, etc) and its commit id (i.e., one line of the
  // git-ls-remote output).
  //
  struct ref
  {
    string name;      // Note: without the peel operation ('^{...}').
    string commit;
    bool   peeled;    // True for '...^{...}' references.
  };

  // List of all refs and their commit ids advertized by a repository (i.e.,
  // the git-ls-remote output).
  //
  class refs: public vector<ref>
  {
  public:
    // Resolve references using a name or a pattern. If requested, also search
    // for abbreviated commit ids unless a matching reference is found, or the
    // argument is a pattern, or it is too short (see rep-add(1) for details).
    // Unless the argument is a pattern, fail if no match is found.
    //
    using search_result = vector<reference_wrapper<const ref>>;

    search_result
    search_names (const string& n, bool abbr_commit) const
    {
      search_result r;
      bool pattern (n.find_first_of ("*?") != string::npos);

      auto search = [this, pattern, &r] (const string& n)
      {
        // Optimize for non-pattern refnames.
        //
        if (pattern)
        {
          path p (n);
          for (const ref& rf: *this)
          {
            if (!rf.peeled && path_match (p, path (rf.name)))
            {
              // Note that the same name can be matched by different patterns
              // (like /refs/** and /refs/tags/**), so we need to suppress
              // duplicates.
              //
              if (find_if (r.begin (), r.end (),
                           [&rf] (const reference_wrapper<const ref>& r)
                           {
                             return &r.get () == &rf;
                           }) == r.end ())
                r.push_back (rf);
            }
          }
        }
        else
        {
          auto i (find_if (begin (), end (),
                           [&n] (const ref& i)
                           {
                             // Note: skip peeled.
                             //
                             return !i.peeled && i.name == n;
                           }));

          if (i != end ())
            r.push_back (*i);
        }
      };

      if (n[0] != '/')              // Relative refname.
      {
        // This handles symbolic references like HEAD.
        //
        if (n.find ('/') == string::npos)
          search (n);

        search ("refs/" + n);
        search ("refs/tags/" + n);
        search ("refs/heads/" + n);
      }
      else                          // Absolute refname.
        search ("refs" + n);

      // See if this is an abbreviated commit id. We do this check if no names
      // found but not for patterns. We also don't bother checking strings
      // shorter than 7 characters (git default).
      //
      const ref* cr;
      if (r.empty () && abbr_commit && !pattern && n.size () >= 7 &&
          (cr = find_commit (n)) != nullptr)
        r.push_back (*cr);

      if (r.empty () && !pattern)
        fail << "reference '" << n << "' is not found";

      return r;
    }

    // Resolve (potentially abbreviated) commit id returning NULL if not
    // found and failing if the resolution is ambiguous.
    //
    const ref*
    find_commit (const string& c) const
    {
      const ref* r (nullptr);
      size_t n (c.size ());

      for (const ref& rf: *this)
      {
        if (rf.commit.compare (0, n, c) == 0)
        {
          if (r == nullptr)
            r = &rf;

          // Note that different names can refer to the same commit.
          //
          else if (r->commit != rf.commit)
            fail << "abbreviated commit id " << c << " is ambiguous" <<
              info << "candidate: " << r->commit <<
              info << "candidate: " << rf.commit;
        }
      }

      return r;
    }
  };

  // Map of repository URLs to their advertized refs/commits.
  //
  using repository_refs_map = map<string, refs>;

  static repository_refs_map repository_refs;

  // It is assumed that sense_capabilities() function was already called for
  // the URL.
  //
  static const refs&
  load_refs (const common_options& co, const repository_url& url)
  {
    tracer trace ("load_refs");

    string u (url.string ());
    auto i (repository_refs.find (u));

    if (i != repository_refs.end ())
      return i->second;

    if (verb)
      text << "querying " << url;

    refs rs;
    fdpipe pipe (open_pipe ());

    process pr (start_git (co,
                           pipe, 2 /* stderr */,
                           timeout_opts (co, url.scheme),
                           co.git_option (),
                           "ls-remote",
                           to_git_url (url)));

    // Shouldn't throw, unless something is severely damaged.
    //
    pipe.out.close ();

    for (;;) // Breakout loop.
    {
      try
      {
        ifdstream is (move (pipe.in), fdstream_mode::skip, ifdstream::badbit);

        for (string l; !eof (getline (is, l)); )
        {
          l4 ([&]{trace << "ref line: " << l;});

          size_t n (l.find ('\t'));

          if (n == string::npos)
            fail << "unable to parse references for " << url << endg;

          string cm (l, 0, n);
          string nm (l, n + 1);

          // Skip the reserved branch prefix.
          //
          if (nm.compare (0, 25, "refs/heads/build2-control") == 0)
            continue;

          n = nm.rfind ("^{");
          bool peeled (n != string::npos);

          if (peeled)
            nm.resize (n); // Strip the peel operation ('^{...}').

          rs.push_back (ref {move (nm), move (cm), peeled});
        }

        is.close ();

        if (pr.wait ())
          break;

        // Fall through.
      }
      catch (const io_error&)
      {
        if (pr.wait ())
          fail << "unable to read references for " << url << endg;

        // Fall through.
      }

      // We should only get here if the child exited with an error status.
      //
      assert (!pr.wait ());

      fail << "unable to list references for " << url << endg;
    }

    return repository_refs.emplace (move (u), move (rs)).first->second;
  }

  // Return true if a commit is advertised by the remote repository. It is
  // assumed that sense_capabilities() function was already called for the URL.
  //
  static bool
  commit_advertized (const common_options& co,
                     const repository_url& url,
                     const string& commit)
  {
    return load_refs (co, url).find_commit (commit) != nullptr;
  }

  // Return true if a commit is already fetched.
  //
  static bool
  commit_fetched (const common_options& co,
                  const dir_path& dir,
                  const string& commit)
  {
    auto_fd dev_null (open_dev_null ());

    process pr (start_git (co,
                           1,                // The output is suppressed by -e.
                           dev_null,
                           co.git_option (),
                           "-C", dir,
                           "cat-file",
                           "-e",
                           commit + "^{commit}"));

    // Shouldn't throw, unless something is severely damaged.
    //
    dev_null.close ();
    return pr.wait ();
  }

  // Create an empty repository and configure the remote origin URL and the
  // default fetch refspec. If requested, use a separate git directory,
  // creating it if absent.
  //
  static void
  init (const common_options& co,
        const dir_path& dir,
        const repository_url& url,
        const dir_path& git_dir = dir_path ())
  {
    if (!run_git (
          co,
          co.git_option (),
          "init",

          !git_dir.empty ()
          ? strings ({"--separate-git-dir=" + git_dir.string ()})
          : strings (),

          verb < 2 ? opt ("-q") : nullopt,
          dir))
      fail << "unable to init " << dir << endg;

    origin_url (co, dir, url);

    config_set (co,
                dir,
                "remote.origin.fetch",
                "+refs/heads/*:refs/remotes/origin/*");
  }

  // Return true if the shallow fetch is possible for the reference.
  //
  static bool
  shallow_fetch (const common_options& co,
                 const repository_url& url,
                 capabilities cap,
                 const git_ref_filter& rf)
  {
    switch (cap)
    {
    case capabilities::dumb:
      {
        return false;
      }
    case capabilities::smart:
      {
        return !rf.commit || commit_advertized (co, url, *rf.commit);
      }
    case capabilities::unadv:
      {
        return true;
      }
    }

    assert (false); // Can't be here.
    return false;
  }

  // Fetch and return repository fragments resolved using the specified
  // repository reference filters.
  //
  static vector<git_fragment>
  fetch (const common_options& co,
         const dir_path& dir,
         const dir_path& submodule,  // Used only for diagnostics.
         const git_ref_filters& rfs)
  {
    assert (!rfs.empty ());

    // We will delay calculating the remote origin URL and/or sensing
    // capabilities until we really need them. Under some plausible scenarios
    // we may do without them.
    //
    repository_url ou;
    optional<capabilities> cap;

    auto url = [&co, &dir, &ou] () -> const repository_url&
    {
      if (ou.empty ())
        ou = origin_url (co, dir);

      return ou;
    };

    auto caps = [&co, &url, &cap] () -> capabilities
    {
      if (!cap)
        cap = sense_capabilities (co, url ());

      return *cap;
    };

    auto references = [&co, &url] (const string& refname, bool abbr_commit)
      -> refs::search_result
    {
      return load_refs (co, url ()).search_names (refname, abbr_commit);
    };

    // Return the default reference set (see add-rep(1) for details).
    //
    auto default_references = [&co, &url] () -> refs::search_result
    {
      refs::search_result r;
      for (const ref& rf: load_refs (co, url ()))
      {
        if (!rf.peeled && rf.name.compare (0, 11, "refs/tags/v") == 0 &&
            parse_standard_version (string (rf.name, 11)))
          r.push_back (rf);
      }

      return r;
    };

    // Return a user-friendly reference name.
    //
    auto friendly_name = [] (const string& n) -> string
    {
      // Strip 'refs/' prefix if present.
      //
      return n.compare (0, 5, "refs/") == 0 ? string (n, 5) : n;
    };

    // Collect the list of commits together with the refspecs that should be
    // used to fetch them. If refspecs are absent then the commit is already
    // fetched (and must not be re-fetched). Otherwise, if it is empty, then
    // the whole repository history must be fetched. And otherwise, it is a
    // list of commit ids.
    //
    // Note that the <refname>@<commit> filter may result in multiple refspecs
    // for a single commit.
    //
    struct fetch_spec
    {
      string commit;
      string friendly_name;
      optional<strings> refspecs;
      bool shallow;               // Meaningless if refspec is absent.
    };

    vector<fetch_spec> fspecs;

    for (const git_ref_filter& rf: rfs)
    {
      // Add/upgrade the fetch specs, minimizing the amount of history to
      // fetch and saving the commit friendly name.
      //
      auto add_spec = [&fspecs] (const string& c,
                                 optional<strings>&& rs = nullopt,
                                 bool sh = false,
                                 string n = string ())
      {
        auto i (find_if (fspecs.begin (), fspecs.end (),
                         [&c] (const fetch_spec& i) {return i.commit == c;}));

        if (i == fspecs.end ())
          fspecs.push_back (fetch_spec {c, move (n), move (rs), sh});
        else
        {
          // No reason to change our mind about (not) fetching.
          //
          assert (static_cast<bool> (rs) == static_cast<bool> (i->refspecs));

          // We always prefer to fetch less history.
          //
          if (rs && ((!rs->empty () && i->refspecs->empty ()) ||
                     (sh && !i->shallow)))
          {
            i->refspecs = move (rs);
            i->shallow = sh;

            if (!n.empty ())
              i->friendly_name = move (n);
          }
          else if (i->friendly_name.empty () && !n.empty ())
            i->friendly_name = move (n);
        }
      };

      // Remove the fetch spec.
      //
      auto remove_spec = [&fspecs] (const string& c)
      {
        auto i (find_if (fspecs.begin (), fspecs.end (),
                         [&c] (const fetch_spec& i) {return i.commit == c;}));

        if (i != fspecs.end ())
          fspecs.erase (i);
      };

      // Evaluate if the commit can be obtained with the shallow fetch. We will
      // delay this evaluation until we really need it. Under some plausible
      // scenarios we may do without it.
      //
      optional<bool> sh;
      auto shallow = [&co, &url, &caps, &rf, &sh] () -> bool
      {
        if (!sh)
          sh = shallow_fetch (co, url (), caps (), rf);

        return *sh;
      };

      // If commit is not specified, then we fetch or exclude commits the
      // refname translates to. Here we also handle the default reference set.
      //
      if (!rf.commit)
      {
        // Refname must be specified, except for the default reference set
        // filter.
        //
        assert (rf.default_refs () || rf.name);

        for (const auto& r: rf.default_refs ()
                            ? default_references ()
                            : references (*rf.name, true /* abbr_commit */))
        {
          const string& c (r.get ().commit);

          if (!rf.exclusion)
          {
            string n (friendly_name (r.get ().name));

            if (commit_fetched (co, dir, c))
              add_spec (
                c, nullopt /* refspecs */, false /* shallow */, move (n));
            else
              add_spec (c, strings ({c}), shallow (), move (n));
          }
          else
            remove_spec (c);
        }
      }
      // Check if this is a commit exclusion and remove the corresponding
      // fetch spec if that's the case.
      //
      else if (rf.exclusion)
        remove_spec (*rf.commit);

      // Check if the commit is already fetched and, if that's the case, save
      // it, indicating that no fetch is required.
      //
      else if (commit_fetched (co, dir, *rf.commit))
        add_spec (*rf.commit);

      // If the shallow fetch is possible for the commit, then we fetch it.
      //
      else if (shallow ())
      {
        assert (!rf.exclusion); // Already handled.

        add_spec (*rf.commit, strings ({*rf.commit}), true);
      }
      // If the shallow fetch is not possible for the commit but the refname
      // containing the commit is specified, then we fetch the whole history
      // of references the refname translates to.
      //
      else if (rf.name)
      {
        assert (!rf.exclusion); // Already handled.

        refs::search_result rs (
          references (*rf.name, false /* abbr_commit */));

        // The resulting set may not be empty. Note that the refname is a
        // pattern, otherwise we would fail earlier (see refs::search_names()
        // function for more details).
        //
        if (rs.empty ())
          fail << "no names match pattern '" << *rf.name << "'";

        strings specs;
        for (const auto& r: rs)
          specs.push_back (r.get ().commit);

        add_spec (*rf.commit, move (specs)); // Fetch deep.
      }
      // Otherwise, if the refname is not specified and the commit is not
      // advertised, we have to fetch the whole repository history.
      //
      else
      {
        assert (!rf.exclusion); // Already handled.

        const string& c (*rf.commit);

        // Fetch deep in both cases.
        //
        add_spec (
          c, commit_advertized (co, url (), c) ? strings ({c}) : strings ());
      }
    }

    // Now save the resulting commit ids and separate the collected refspecs
    // into the deep and shallow fetch lists.
    //
    vector<git_fragment> r;

    strings scs; // Shallow fetch commits.
    strings dcs; // Deep fetch commits.

    // Fetch the whole repository history.
    //
    bool fetch_repo (false);

    for (fetch_spec& fs: fspecs)
    {
      // Fallback to the abbreviated commit for the friendly name.
      //
      string n (!fs.friendly_name.empty ()
                ? move (fs.friendly_name)
                : string (fs.commit, 0, 12));

      // We will fill timestamps later, after all the commits are fetched.
      //
      r.push_back (
        git_fragment {move (fs.commit), 0 /* timestamp */, move (n)});

      // Save the fetch refspecs to the proper list.
      //
      if (fs.refspecs)
      {
        // If we fetch the whole repository history, then no refspecs is
        // required, so we stop collecting them if that's the case.
        //
        if (fs.refspecs->empty ())
          fetch_repo = true;
        else if (!fetch_repo)
        {
          strings& cs (fs.shallow ? scs : dcs);
          for (string& s: *fs.refspecs)
            cs.push_back (move (s));
        }
      }
    }

    // Set timestamps for commits and sort them in the timestamp ascending
    // order.
    //
    auto sort = [&co, &dir] (vector<git_fragment>&& fs) -> vector<git_fragment>
    {
      for (git_fragment& fr: fs)
      {
        // Add '^{commit}' suffix to strip some unwanted output that appears
        // for tags.
        //
        string s (git_line (co, "commit timestamp",
                            co.git_option (),
                            "-C", dir,
                            "show",
                            "-s",
                            "--format=%ct",
                            fr.commit + "^{commit}"));
        try
        {
          fr.timestamp = static_cast<time_t> (stoull (s));
        }
        // Catches both std::invalid_argument and std::out_of_range that
        // inherit from std::logic_error.
        //
        catch (const logic_error&)
        {
          fail << "'" << s << "' doesn't appear to contain a git commit "
            "timestamp" << endg;
        }
      }

      std::sort (fs.begin (), fs.end (),
                 [] (const git_fragment& x, const git_fragment& y)
                 {
                   return x.timestamp < y.timestamp;
                 });

      return move (fs);
    };

    // Bail out if all commits are already fetched.
    //
    if (!fetch_repo && scs.empty () && dcs.empty ())
      return sort (move (r));

    // Fetch the refspecs. If no refspecs are specified, then fetch the
    // whole repository history.
    //
    auto fetch = [&co, &url, &dir] (const strings& refspecs, bool shallow)
    {
      // We don't shallow fetch the whole repository.
      //
      assert (!refspecs.empty () || !shallow);

      // Prior to 2.14.0 the git-fetch command didn't accept commit id as a
      // refspec:
      //
      // $ git fetch --no-recurse-submodules --depth 1 origin 5e8245ee3526530a3467f59b0601bbffb614f45b
      //   error: Server does not allow request for unadvertised object 5e8245ee3526530a3467f59b0601bbffb614f45b
      //
      // We will try to remap commits back to git refs (tags, branches, etc)
      // based on git-ls-remote output and fail if unable to do so (which
      // should only happen for unadvertised commits).
      //
      // Note that in this case we will fail only for servers supporting
      // unadvertised refs fetch. For other protocols we have already fallen
      // back to fetching some history, passing to fetch() either advertised
      // commit ids (of branches, tags, etc) or an empty refspecs list (the
      // whole repository history). So we could just reduce the server
      // capabilities from 'unadv' to 'smart' for such old clients.
      //
      optional<strings> remapped_refspecs;
      if (!refspecs.empty () && git_ver < semantic_version {2, 14, 0})
      {
        remapped_refspecs = strings ();

        for (const string& c: refspecs)
        {
          const ref* r (load_refs (co, url ()).find_commit (c));

          if (r == nullptr)
            fail << "git version is too old for specified location" <<
              info << "consider upgrading git to 2.14.0 or above";

          remapped_refspecs->push_back (r->name);
        }
      }

      // Note that we suppress the (too detailed) fetch command output if the
      // verbosity level is 1. However, we still want to see the progress in
      // this case, unless stderr is not directed to a terminal.
      //
      // Also note that we don't need to specify --refmap option since we can
      // rely on the init() function that properly sets the
      // remote.origin.fetch configuration option.
      //
      if (!run_git (co,
                    timeout_opts (co, url ().scheme),
                    co.git_option (),
                    "-C", dir,
                    "fetch",
                    "--no-recurse-submodules",
                    shallow ? cstrings ({"--depth", "1"}) : cstrings (),
                    verb == 1 && fdterm (2) ? opt ("--progress") : nullopt,
                    verb < 2 ? opt ("-q") : verb > 3 ? opt ("-v") : nullopt,
                    "origin",
                    remapped_refspecs ? *remapped_refspecs : refspecs))
        fail << "unable to fetch " << dir << endg;
    };

    // Print the progress indicator.
    //
    // Note that the clone command prints the following line prior to the
    // progress lines:
    //
    // Cloning into '<dir>'...
    //
    // The fetch command doesn't print anything similar, for some reason.
    // This makes it hard to understand which superproject/submodule is
    // currently being fetched. Let's fix that.
    //
    // Also note that we have "fixed" that capital letter nonsense and stripped
    // the trailing '...'.
    //
    if (verb)
    {
      diag_record dr (text);
      dr << "fetching ";

      if (!submodule.empty ())
        dr << "submodule '" << submodule.posix_string () << "' ";

      dr << "from " << url ();

      if (verb >= 2)
        dr << " in '" << dir.posix_string () << "'"; // Is used by tests.
    }

    // First, we perform the deep fetching.
    //
    if (fetch_repo || !dcs.empty ())
    {
      // Print warnings prior to the deep fetching.
      //
      {
        diag_record dr (warn);
        dr << "fetching whole " << (fetch_repo ? "repository" : "reference")
           << " history";

        if (!submodule.empty ())
          dr << " for submodule '" << submodule.posix_string () << "'";

        dr << " ("
           << (caps () == capabilities::dumb
               ? "dumb HTTP"
               : "unadvertised commit") // There are no other reasons so far.
           << ')';
      }

      if (caps () == capabilities::dumb)
        warn << "no progress will be shown (dumb HTTP)";

      // Fetch.
      //
      fetch (fetch_repo ? strings () : dcs, false);

      // After the deep fetching some of the shallow commits might also be
      // fetched, so we drop them from the fetch list.
      //
      for (auto i (scs.begin ()); i != scs.end (); )
      {
        if (commit_fetched (co, dir, *i))
          i = scs.erase (i);
        else
          ++i;
      }
    }

    // Finally, we perform the shallow fetching.
    //
    if (!scs.empty ())
      fetch (scs, true);

    // We also need to make sure that all the resulting commits are now
    // fetched. This may not be the case if the user misspelled the
    // [<refname>@]<commit> filter.
    //
    for (const git_fragment& fr: r)
    {
      if (!commit_fetched (co, dir, fr.commit))
        fail << "unable to fetch commit " << fr.commit;
    }

    return sort (move (r));
  }

  // Checkout the repository submodules (see git_checkout_submodules()
  // description for details).
  //
  static void
  checkout_submodules (const common_options& co,
                       const dir_path& dir,
                       const dir_path& git_dir,
                       const dir_path& prefix)
  {
    tracer trace ("checkout_submodules");

    path mf (dir / path (".gitmodules"));

    if (!exists (mf))
      return;

    auto failure = [&prefix] (const char* desc)
    {
      diag_record dr (fail);
      dr << desc;

      if (!prefix.empty ())
        // Strips the trailing slash.
        //
        dr << " for submodule '" << prefix.string () << "'";

      dr << endg;
    };

    // Initialize submodules.
    //
    if (!run_git (
          co,
          co.git_option (),
          "-C", dir,

          // Note that older git versions don't recognize the --super-prefix
          // option but seem to behave correctly without any additional
          // efforts when it is omitted.
          //
          !prefix.empty () && git_ver >= semantic_version {2, 14, 0}
          ? strings ({"--super-prefix", prefix.posix_representation ()})
          : strings (),

          "submodule--helper", "init",
          verb < 2 ? opt ("-q") : nullopt))
      failure ("unable to initialize submodules");

    repository_url orig_url (origin_url (co, dir));

    // Iterate over the registered submodules initializing/fetching them and
    // recursively checking them out.
    //
    // Note that we don't expect submodules nesting be too deep and so recurse
    // while reading the git process output.
    //
    // Also note that we don't catch the failed exception here, relying on the
    // fact that the process destructor will wait for the process completion.
    //
    fdpipe pipe (open_pipe ());

    process pr (start_git (co,
                           pipe, 2 /* stderr */,
                           co.git_option (),
                           "-C", dir,
                           "submodule--helper", "list"));

    // Shouldn't throw, unless something is severely damaged.
    //
    pipe.out.close ();

    try
    {
      ifdstream is (move (pipe.in), fdstream_mode::skip, ifdstream::badbit);

      for (string l; !eof (getline (is, l)); )
      {
        // The line describing a submodule has the following form:
        //
        // <mode><SPACE><commit><SPACE><stage><TAB><path>
        //
        // For example:
        //
        // 160000 658436a9522b5a0d016c3da0253708093607f95d 0	doc/style
        //
        l4 ([&]{trace << "submodule: " << l;});

        if (!(l.size () > 50 && l[48] == '0' && l[49] == '\t'))
          failure ("invalid submodule description");

        string commit (l.substr (7, 40));

        // Submodule directory path, relative to the containing project.
        //
        dir_path sdir  (l.substr (50));

        // Submodule directory path, relative to the top project.
        //
        dir_path psdir (prefix / sdir);
        string psd (psdir.posix_string ()); // For use in the diagnostics.

        string nm (git_line (co, "submodule name",
                             co.git_option (),
                             "-C", dir,
                             "submodule--helper", "name",
                             sdir));

        string uo ("submodule." + nm + ".url");
        string uv (config_get (co, dir, uo, "submodule URL"));

        l4 ([&]{trace << "name: " << nm << ", URL: " << uv;});

        dir_path fsdir (dir / sdir);
        bool initialized (git_repository (fsdir));

        // If the submodule is already initialized and its commit didn't
        // change then we skip it.
        //
        if (initialized && git_line (co, "submodule commit",
                                     co.git_option (),
                                     "-C", fsdir,
                                     "rev-parse",
                                     "--verify",
                                     "HEAD") == commit)
          continue;

        // Note that the "submodule--helper init" command (see above) doesn't
        // sync the submodule URL in .git/config file with the one in
        // .gitmodules file, that is a primary URL source. Thus, we always
        // calculate the URL using .gitmodules and update it in .git/config, if
        // necessary.
        //
        repository_url url;

        try
        {
          url = from_git_url (
            config_get (co, mf, uo, "submodule original URL"));

          // Complete the relative submodule URL against the containing
          // repository origin URL.
          //
          if (url.scheme == repository_protocol::file && url.path->relative ())
          {
            repository_url u (orig_url);
            *u.path /= *url.path;

            // Note that we need to collapse 'example.com/a/..' to
            // 'example.com/', rather than to 'example.com/.'.
            //
            u.path->normalize (
              false /* actual */,
              orig_url.scheme != repository_protocol::file /* cur_empty */);

            url = move (u);
          }

          // Fix-up submodule URL in .git/config file, if required.
          //
          if (url != from_git_url (move (uv)))
          {
            config_set (co, dir, uo, to_git_url (url));

            // We also need to fix-up submodule's origin URL, if its
            // repository is already initialized.
            //
            if (initialized)
              origin_url (co, fsdir, url);
          }
        }
        catch (const invalid_path& e)
        {
          fail << "invalid repository path for submodule '" << psd << "': "
               << e << endg;
        }
        catch (const invalid_argument& e)
        {
          fail << "invalid repository URL for submodule '" << psd << "': "
               << e << endg;
        }

        // Initialize the submodule repository.
        //
        // Note that we initialize the submodule repository git directory out
        // of the working tree, the same way as "submodule--helper clone"
        // does. This prevents us from loosing the fetched data when switching
        // the containing repository between revisions, that potentially
        // contain different sets of submodules.
        //
        dir_path gdir (git_dir / dir_path ("modules") / sdir);

        if (!initialized)
        {
          mk_p (gdir);
          init (co, fsdir, url, gdir);
        }

        // Fetch and checkout the submodule.
        //
        git_ref_filters rfs {
          git_ref_filter {nullopt, commit, false /* exclusion */}};

        fetch (co, fsdir, psdir, rfs);

        git_checkout (co, fsdir, commit);

        // Let's make the message match the git-submodule script output
        // (again, except for capitalization).
        //
        if (verb)
          text << "submodule path '" << psd << "': checked out '" << commit
               << "'";

        // Check out the submodule submodules, recursively.
        //
        checkout_submodules (co, fsdir, gdir, psdir);
      }

      is.close ();

      if (pr.wait ())
        return;

      // Fall through.
    }
    catch (const io_error&)
    {
      if (pr.wait ())
        failure ("unable to read submodules list");

      // Fall through.
    }

    // We should only get here if the child exited with an error status.
    //
    assert (!pr.wait ());

    failure ("unable to list submodules");
  }

  void
  git_init (const common_options& co,
            const repository_location& rl,
            const dir_path& dir)
  {
    repository_url url (rl.url ());
    url.fragment = nullopt;

    init (co, dir, url);
  }

  // Update the repository remote origin URL, if changed.
  //
  static void
  sync_origin_url (const common_options& co,
                   const repository_location& rl,
                   const dir_path& dir)
  {
    repository_url url (rl.url ());
    url.fragment = nullopt;

    repository_url u (origin_url (co, dir));

    if (url != u)
    {
      // Note that the repository canonical name with the fragment part
      // stripped can not change under the legal scenarios that lead to the
      // location change. Changed canonical name means that the repository was
      // manually amended. We could fix-up such repositories as well but want
      // to leave the backdoor for tests.
      //
      if (repository_location (url, rl.type ()).canonical_name () ==
          repository_location (u,   rl.type ()).canonical_name ())
      {
        if (verb)
        {
          u.fragment = rl.url ().fragment; // Restore the fragment.

          info << "location changed for " << rl.canonical_name () <<
            info << "new location " << rl <<
            info << "old location " << repository_location (u, rl.type ());
        }

        origin_url (co, dir, url);
      }
    }
  }

  vector<git_fragment>
  git_fetch (const common_options& co,
             const repository_location& rl,
             const dir_path& dir)
  {
    git_ref_filters rfs;
    const repository_url& url (rl.url ());

    try
    {
      rfs = parse_git_ref_filters (url.fragment);
    }
    catch (const invalid_argument& e)
    {
      fail << "unable to fetch " << url << ": " << e;
    }

    sync_origin_url (co, rl, dir);
    return fetch (co, dir, dir_path () /* submodule */, rfs);
  }

  void
  git_checkout (const common_options& co,
                const dir_path& dir,
                const string& commit)
  {
    // For some (probably valid) reason the hard reset command doesn't remove
    // a submodule directory that is not plugged into the project anymore. It
    // also prints the non-suppressible warning like this:
    //
    // warning: unable to rmdir libbar: Directory not empty
    //
    // That's why we run the clean command afterwards. It may also be helpful
    // if we produce any untracked files in the tree between checkouts down
    // the road.
    //
    if (!run_git (
          co,
          co.git_option (),
          "-C", dir,
          "reset",
          "--hard",
          verb < 2 ? opt ("-q") : nullopt,
          commit))
      fail << "unable to reset to " << commit << endg;

    if (!run_git (
          co,
          co.git_option (),
          "-C", dir,
          "clean",
          "-d",
          "-x",
          "-ff",
          verb < 2 ? opt ("-q") : nullopt))
      fail << "unable to clean " << dir << endg;
  }

  void
  git_checkout_submodules (const common_options& co,
                           const repository_location& rl,
                           const dir_path& dir)
  {
    // Note that commits could come from different repository URLs that may
    // contain different sets of commits. Thus, we need to switch to the URL
    // the checked out commit came from to properly complete submodule
    // relative URLs.
    //
    sync_origin_url (co, rl, dir);

    checkout_submodules (co,
                         dir,
                         dir / dir_path (".git"),
                         dir_path () /* prefix */);
  }
}
