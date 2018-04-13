// file      : bpkg/fetch-git.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch.hxx>

#include <map>
#include <algorithm> // find(), find_if(), replace()

#include <libbutl/utility.mxx>          // digit(), xdigit()
#include <libbutl/process.mxx>
#include <libbutl/standard-version.mxx>

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
    case repository_protocol::file: return strings (); // Local communications.
    }

    assert (false); // Can't be here.
    return strings ();
  }

  template <typename... A>
  static string
  git_string (const common_options&, const char* what, A&&... args);

  // Start git process. On the first call check that git version is 2.12.0 or
  // above, and fail if that's not the case.
  //
  // Note that git is executed in the "sanitized" environment, having the
  // environment variables that are local to the repository being unset (all
  // except GIT_CONFIG_PARAMETERS). We do the same as the git-submodule script
  // does for commands executed for submodules. Though we do it for all
  // commands (including the ones related to the top repository).
  //
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
          string s (git_string (co, "git version",
                                co.git_option (),
                                "--version"));

          standard_version v;

          // There is some variety across platforms in the version
          // representation.
          //
          // Linux:  git version 2.14.3
          // MacOS:  git version 2.10.1 (Apple Git-78)
          // MinGit: git version 2.16.1.windows.1
          //
          // We will consider the first 3 version components that follows the
          // common 'git version ' prefix.
          //
          const size_t b (12);
          if (s.compare (0, b, "git version ") == 0)
          {
            size_t i (b);
            size_t n (0);
            for (char c; i != s.size () && (digit (c = s[i]) || c == '.'); ++i)
            {
              if (c == '.' && ++n == 3)
                break;
            }

            try
            {
              v = standard_version (string (s, b, i - b));
            }
            catch (const invalid_argument&) {}
          }

          if (v.empty ())
            fail << "'" << s << "' doesn't appear to contain a git version" <<
              info << "produced by '" << co.git () << "'; "
                 << "use --git to override" << endg;

          if (v.version < 20120000000)
            fail << "unsupported git version " << v.string () <<
              info << "minimum supported version is 2.12.0" << endf;

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
  git_string (const common_options& co, const char* what, A&&... args)
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
    return git_string (co,
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
    return git_string (co,
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
  // git://).
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
    string name;      // Note: without the peel marker ('^{}').
    string commit;
    bool   peeled;    // True for '...^{}' references.
  };

  // List of all refs and their commit ids advertized by a repository (i.e.,
  // the git-ls-remote output).
  //
  class refs: public vector<ref>
  {
  public:
    // Resolve a git refname (tag, branch, etc) returning NULL if not found.
    //
    const ref*
    find_name (const string& n, bool abbr_commit) const
    {
      auto find = [this] (const string& n) -> const ref*
      {
        auto i (find_if (
                  begin (), end (),
                  [&n] (const ref& i)
                  {
                    return !i.peeled && i.name == n; // Note: skip peeled.
                  }));

        return i != end () ? &*i : nullptr;
      };

      // Let's search in the order refs are disambiguated by git (see
      // gitrevisions(7) for details).
      //
      // What should we do if the name already contains a slash, for example,
      // tags/v1.2.3? Note that while it most likely is relative to refs/, it
      // can also be relative to other subdirectories (say releases/v1.2.3 in
      // tags/releases/v1.2.3). So we just check everywhere.

      // This handles symbolic references like HEAD.
      //
      if (const ref* r = find (n))
        return r;

      if (const ref* r = find ("refs/" + n))
        return r;

      if (const ref* r = find ("refs/tags/" + n))
        return r;

      if (const ref* r = find ("refs/heads/" + n))
        return r;

      // See if this is an abbreviated commit id. We do this check last since
      // this can be ambiguous. We also don't bother checking strings shorter
      // than 7 characters (git default).
      //
      return abbr_commit && n.size () >= 7 ? find_commit (n) : nullptr;
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

  // @@ Move to libbpkg when adding support for multiple fragments.
  //
  using git_ref_filters = vector<git_ref_filter>;

  // Fetch the references and return their commit ids. If there are no
  // reference filters specified, then fetch all the tags and branches.
  //
  static strings
  fetch (const common_options& co,
         const dir_path& dir,
         const dir_path& submodule,  // Used only for diagnostics.
         const git_ref_filters& rfs)
  {
    strings r;
    capabilities cap;
    repository_url url;

    strings scs;  // Shallow fetch commits.
    strings dcs;  // Deep fetch commits.

    bool fetch_repo (false);

    // Translate a name to the corresponding commit. Fail if the name is not
    // known by the remote repository.
    //
    auto commit = [&co, &dir, &url] (const string& nm, bool abbr_commit)
      -> const string&
    {
      if (url.empty ())
        url = origin_url (co, dir);

      const ref* r (load_refs (co, url).find_name (nm, abbr_commit));

      if (r == nullptr)
        fail << "unable to fetch " << nm << " from " << url <<
          info << "name is not recognized" << endg;

      return r->commit;
    };

    // Add a commit to the list, suppressing duplicates.
    //
    auto add = [] (const string& c, strings& cs)
    {
      if (find (cs.begin (), cs.end (), c) == cs.end ())
        cs.push_back (c);
    };

    if (rfs.empty ())
    {
      url = origin_url (co, dir);

      for (const ref& rf: load_refs (co, url))
      {
        // Skip everything other than tags and branches.
        //
        if (rf.peeled ||
            (rf.name.compare (0, 11, "refs/heads/") != 0 &&
             rf.name.compare (0, 10, "refs/tags/") != 0))
          continue;

        // Add an already fetched commit to the resulting list.
        //
        if (commit_fetched (co, dir, rf.commit))
        {
          add (rf.commit, r);
          continue;
        }

        // Evaluate if the commit can be obtained with the shallow fetch and
        // add it to the proper list.
        //
        if (scs.empty () && dcs.empty ())
          cap = sense_capabilities (co, url);

        // The commit is advertised, so we can fetch shallow, unless the
        // protocol is dumb.
        //
        add (rf.commit, cap != capabilities::dumb ? scs : dcs);
      }
    }
    else
    {
      for (const git_ref_filter& rf: rfs)
      {
        // Add an already fetched commit to the resulting list.
        //
        if (rf.commit && commit_fetched (co, dir, *rf.commit))
        {
          add (*rf.commit, r);
          continue;
        }

        if (!rf.commit)
        {
          assert (rf.name);

          const string& c (commit (*rf.name, true /* abbr_commit */));

          if (commit_fetched (co, dir, c))
          {
            add (c, r);
            continue;
          }
        }

        // Evaluate if the commit can be obtained with the shallow fetch and
        // add it to the proper list.
        //
        if (scs.empty () && dcs.empty ())
        {
          if (url.empty ()) // Can already be assigned by commit() lambda.
            url = origin_url (co, dir);

          cap = sense_capabilities (co, url);
        }

        bool shallow (shallow_fetch (co, url, cap, rf));
        strings& commits (shallow ? scs : dcs);

        // If commit is not specified, then we fetch the commit the refname
        // translates to.
        //
        if (!rf.commit)
        {
          assert (rf.name);

          add (commit (*rf.name, true /* abbr_commit */), commits);
        }

        // If commit is specified and the shallow fetch is possible, then we
        // fetch the commit.
        //
        else if (shallow)
          add (*rf.commit, commits);

        // If commit is specified and the shallow fetch is not possible, but
        // the refname containing the commit is specified, then we fetch the
        // whole refname history.
        //
        else if (rf.name)
          add (commit (*rf.name, false /* abbr_commit */), commits);

        // Otherwise, if the refname is not specified and the commit is not
        // advertised, we have to fetch the whole repository history.
        //
        else
        {
          add (*rf.commit, commits);
          fetch_repo = !commit_advertized (co, url, *rf.commit);
        }
      }
    }

    // Bail out if all commits are already fetched.
    //
    if (scs.empty () && dcs.empty ())
      return r;

    auto fetch = [&co, &url, &dir] (const strings& refspecs, bool shallow)
    {
      // Note that we suppress the (too detailed) fetch command output if the
      // verbosity level is 1. However, we still want to see the progress in
      // this case, unless STDERR is not directed to a terminal.
      //
      // Also note that we don't need to specify --refmap option since we can
      // rely on the init() function that properly sets the
      // remote.origin.fetch configuration option.
      //
      if (!run_git (co,
                    timeout_opts (co, url.scheme),
                    co.git_option (),
                    "-C", dir,
                    "fetch",
                    "--no-recurse-submodules",
                    shallow ? cstrings ({"--depth", "1"}) : cstrings (),
                    verb == 1 && fdterm (2) ? opt ( "--progress") : nullopt,
                    verb < 2 ? opt ("-q") : verb > 3 ? opt ("-v") : nullopt,
                    "origin",
                    refspecs))
        fail << "unable to fetch " << dir << endg;
    };

    // Print warnings prior to the deep fetching.
    //
    if (!dcs.empty ())
    {
      {
        diag_record dr (warn);
        dr << "fetching whole " << (fetch_repo ? "repository" : "reference")
           << " history";

        if (!submodule.empty ())
          dr << " for submodule '" << submodule.posix_string () << "'";

        dr << " ("
           << (cap == capabilities::dumb
               ? "dumb HTTP"
               : "unadvertised commit") // There are no other reasons so far.
           << ')';
      }

      if (cap == capabilities::dumb)
        warn << "fetching over dumb HTTP, no progress will be shown";
    }

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

      dr << "from " << url;

      if (verb >= 2)
        dr << " in '" << dir.posix_string () << "'"; // Is used by tests.
    }

    // Note that the shallow, deep and the resulting lists don't overlap.
    // Thus, we will be moving commits between the lists not caring about
    // suppressing duplicates.
    //

    // First, we fetch deep commits.
    //
    if (!dcs.empty ())
    {
      fetch (!fetch_repo ? dcs : strings (), false);

      r.insert (r.end (),
                make_move_iterator (dcs.begin ()),
                make_move_iterator (dcs.end ()));
    }

    // After the deep fetching some of the shallow commits might also be
    // fetched, so we move them to the resulting list.
    //
    strings cs;
    for (auto& c: scs)
      (commit_fetched (co, dir, c) ? r : cs).push_back (move (c));

    // Finally, fetch shallow commits.
    //
    if (!cs.empty ())
    {
      fetch (cs, true);

      r.insert (r.end (),
                make_move_iterator (cs.begin ()),
                make_move_iterator (cs.end ()));
    }

    return r;
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

          !prefix.empty ()
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

        string nm (git_string (co, "submodule name",
                               co.git_option (),
                               "-C", dir,
                               "submodule--helper", "name",
                               sdir));

        string uo ("submodule." + nm + ".url");
        string uv (config_get (co, dir, uo, "submodule URL"));

        l4 ([&]{trace << "name: " << nm << ", URL: " << uv;});

        dir_path fsdir (dir / sdir);
        bool initialized (exists (fsdir / path (".git")));

        // If the submodule is already initialized and its commit didn't
        // change then we skip it.
        //
        if (initialized && git_string (co, "submodule commit",
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
        catch (const invalid_argument& e)
        {
          fail << "invalid repository URL for submodule '" << psd << "': "
               << e << endg;
        }
        catch (const invalid_path& e)
        {
          fail << "invalid repository path for submodule '" << psd << "': "
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
        git_ref_filters rfs {git_ref_filter {nullopt, commit}};
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

  strings
  git_fetch (const common_options& co,
             const repository_location& rl,
             const dir_path& dir)
  {
    git_ref_filters refs;
    repository_url url (rl.url ());

    if (url.fragment)
    try
    {
      refs.emplace_back (*url.fragment);
      url.fragment = nullopt;
    }
    catch (const invalid_argument& e)
    {
      fail << "unable to fetch " << url << ": " << e;
    }

    // Update the repository URL, if changed.
    //
    repository_url u (origin_url (co, dir));

    if (url != u)
    {
      // Note that the repository canonical name can not change under the
      // legal scenarios that lead to the location change. Changed canonical
      // name means that the repository was manually amended. We could fix-up
      // such repositories as well but want to leave the backdoor for tests.
      //
      u.fragment = rl.url ().fragment;       // Restore the fragment.
      repository_location l (u, rl.type ());

      if (rl.canonical_name () == l.canonical_name ())
      {
        if (verb)
          info << "location changed for " << rl.canonical_name () <<
            info << "new location " << rl <<
            info << "old location " << l;

        origin_url (co, dir, url);
      }
    }

    return fetch (co, dir, dir_path () /* submodule */, refs);
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
  git_checkout_submodules (const common_options& co, const dir_path& dir)
  {
    checkout_submodules (co,
                         dir,
                         dir / dir_path (".git"),
                         dir_path () /* prefix */);
  }
}
