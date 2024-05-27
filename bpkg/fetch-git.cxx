// file      : bpkg/fetch-git.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch.hxx>

#include <map>

#include <libbutl/git.hxx>
#include <libbutl/filesystem.hxx>       // path_entry(), try_rmsymlink()
#include <libbutl/path-pattern.hxx>
#include <libbutl/semantic-version.hxx>
#include <libbutl/standard-version.hxx> // parse_standard_version()

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
        return strings ();
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
        return strings ();
      }
    case repository_protocol::file: return strings (); // Local communications.
    }

    assert (false); // Can't be here.
    return strings ();
  }

  // Run git process and return its output as a string if git exits with zero
  // code and nullopt if it exits with the specified "no-result" code. Fail if
  // the output doesn't contain a single line.
  //
  // Note that the zero no-result code means that the result is non-optional.
  // While a non-zero no-result code means that the requested string (for
  // example configuration option value) is not available if git exits with
  // this code.
  //
  template <typename... A>
  static optional<string>
  git_line (const common_options&,
            int no_result,
            const char* what,
            A&&... args);

  template <typename... A>
  inline static string
  git_line (const common_options& co, const char* what, A&&... args)
  {
    return *git_line (co, 0 /* no_result */, what, forward<A> (args)...);
  }

  // Start git process. On the first call check that git version is 2.11.0 or
  // above, and fail if that's not the case. Note that supporting earlier
  // versions doesn't seem worth it (plus other parts of the toolchain also
  // require 2.11.0).
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

      // On startup git prepends the PATH environment variable value with the
      // computed directory path where its sub-programs are supposedly located
      // (--exec-path option, GIT_EXEC_PATH environment variable, etc; see
      // cmd_main() in git's git.c for details).
      //
      // Then, when git needs to run itself or one of its components as a
      // child process, it resolves the full executable path searching in
      // directories listed in PATH (see locate_in_PATH() in git's
      // run-command.c for details).
      //
      // On Windows we install git and its components into a place where it is
      // not expected to be, which results in the wrong path in PATH as set by
      // git (for example, c:/build2/libexec/git-core) which in turn may lead
      // to running some other git that appear in the PATH variable. To
      // prevent this we pass the git's exec directory via the --exec-path
      // option explicitly.
      //
      string ep;
      process_path pp (process::path_search (co.git (), true /* init */));

#ifdef _WIN32
      ep = "--exec-path=" + pp.effect.directory ().string ();
#endif

      return process_start_callback ([] (const char* const args[], size_t n)
                                     {
                                       if (verb >= 2)
                                         print_process (args, n);
                                     },
                                     0 /* stdin */, out, err,
                                     process_env (pp, *unset_vars),
                                     !ep.empty () ? ep.c_str () : nullptr,
                                     forward<A> (args)...);
    }
    catch (const process_error& e)
    {
      fail << "unable to execute " << co.git () << ": " << e << endg;
    }
  }

  // Run git process, optionally suppressing progress.
  //
  template <typename... A>
  static process_exit
  run_git (const common_options& co, bool progress, A&&... args)
  {
    // Unfortunately git doesn't have any kind of a no-progress option. The
    // only way to suppress progress is to run quiet (-q) which also
    // suppresses some potentially useful information. However, git suppresses
    // progress automatically if its stderr is not a terminal. So we use this
    // feature for the progress suppression by redirecting git's stderr to our
    // own diagnostics stream via a proxy pipe.
    //
    fdpipe pipe;

    if (!progress)
      pipe = open_pipe ();

    int err (!progress ? pipe.out.get () : 2);

    // We don't expect git to print anything to stdout, as the caller would use
    // start_git() and pipe otherwise. Thus, let's redirect stdout to stderr
    // for good measure, as git is known to print some informational messages
    // to stdout.
    //
    process pr (start_git (co,
                           err /* stdout */,
                           err /* stderr */,
                           forward<A> (args)...));

    if (!progress)
    {
      // Shouldn't throw, unless something is severely damaged.
      //
      pipe.out.close ();

      try
      {
        dump_stderr (move (pipe.in));

        // Fall through.
      }
      catch (const io_error& e)
      {
        // Fail if git exited normally with zero code, so the issue won't go
        // unnoticed. Otherwise, let the caller handle git's failure.
        //
        if (pr.wait ())
          fail << "unable to read git diagnostics: " << e;

        // Fall through.
      }
    }

    pr.wait ();
    return *pr.exit;
  }

  template <typename... A>
  inline static process_exit
  run_git (const common_options& co, A&&... args)
  {
    return run_git (co, true /* progress */, forward<A> (args)...);
  }

  template <typename... A>
  static optional<string>
  git_line (const common_options& co,
            int no_result,
            const char* what,
            A&&... args)
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

      assert (pr.exit);

      if (pr.exit->normal () && pr.exit->code () == no_result)
        return nullopt;

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
  // @@ An update: we don't use the 'submodule--helper clone' command anymore.
  //    Should we switch to the local path notation for the file:// protocol?
  //
  static string
  to_git_url (const repository_url& url)
  {
    if (url.scheme != repository_protocol::file)
      return url.string ();

#ifndef _WIN32
    // Enforce the 'file://' notation for local URLs (see
    // libbpkg/manifest.hxx).
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
    if (icasecmp (u, "file://", 7) == 0 && u[7] != '/')
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
    if (!run_git (co, co.git_option (), "-C", dir, "config", key, value))
      fail << "unable to set configuration option " << key << "='" << value
           << "' in " << dir << endg;
  }

  // Get option from the specified configuration file.
  //
  inline static optional<string>
  config_get (const common_options& co,
              const path& file,
              const string& key,
              bool required,
              const char* what)
  {
    // Note: `git config --get` command exits with code 1 if the key wasn't
    // found in the configuration file (see git-config(1) for details).
    //
    return git_line (co,
                     required ? 0 : 1 /* no_result */,
                     what,
                     co.git_option (),
                     "config",
                     "--file", file,
                     "--get",
                     key);
  }

  inline static string
  config_get (const common_options& co,
              const path& file,
              const string& key,
              const char* what)
  {
    return *config_get (co, file, key, true /* required */, what);
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
  // differs from 200 and 401, or reading the response body failed. If the
  // response code is 401 (requires authentication), then consider protocol as
  // smart. The thinking here is that a git repository with support for
  // authentication is likely one of the hosting places (like git{hub,lab})
  // and is unlikely to be dumb.
  //
  // Note that, as a side-effect, this function checks the HTTP(S) server
  // availability and so must be called prior to any git command that involves
  // communication to the remote server. Not doing so may result in the command
  // hanging indefinitely while trying to establish TCP/IP connection (see the
  // timeout_opts() function for the gory details).
  //
  // Note that some smart HTTP(S) repositories are capable of adding missing
  // .git directory extension in the URL (see git-upload-pack(1) for details).
  // Some of them, specifically hosted on GitHub, do that if `git/...` value
  // is specified for the User-Agent HTTP request header. We will pretend to
  // be git while sensing the protocol capabilities to "fix-up" repository
  // URLs, if possible. That's why the function requires the git version
  // parameter.
  //
  using capabilities = git_protocol_capabilities;

  static capabilities
  sense_capabilities (const common_options& co,
                      const repository_url& repo_url,
                      const semantic_version& git_ver)
  {
    assert (repo_url.path);

    switch (repo_url.scheme)
    {
    case repository_protocol::git:
    case repository_protocol::ssh:
    case repository_protocol::file: return capabilities::smart;
    case repository_protocol::http:
    case repository_protocol::https: break; // Ask the server (see below).
    }

    // Craft the URL for sensing the capabilities.
    //
    repository_url url (repo_url);
    path& up (*url.path);

    if (!up.to_directory ())
      up = path_cast<dir_path> (move (up));

    up /= path ("info/refs");

    if (url.query)
      *url.query += "&service=git-upload-pack";
    else
      url.query = "service=git-upload-pack";

    string u (url.string ());

    // Start fetching, also trying to retrieve the HTTP status code.
    //
    // We unset failbit to properly handle an empty response (no refs) from
    // the dumb server.
    //
    ifdstream is (ifdstream::badbit);

    pair<process, uint16_t> ps (
      start_fetch_http (co,
                        u,
                        is /* out */,
                        fdstream_mode::skip | fdstream_mode::binary,
                        stderr_mode::redirect_quiet,
                        "git/" + git_ver.string ()));

    process& pr (ps.first);

    // If the fetch program stderr is redirected, then read it out and pass
    // through.
    //
    auto dump_stderr = [&pr] ()
    {
      if (pr.in_efd != nullfd)
      try
      {
        bpkg::dump_stderr (move (pr.in_efd));
      }
      catch (const io_error&)
      {
        // Not much we can do here.
      }
    };

    try
    {
      // If authentication is required (HTTP status code is 401), then
      // consider the protocol as smart. Drop the diagnostics if that's the
      // case and dump it otherwise.
      //
      if (ps.second == 401)
      {
        if (verb >= 2)
        {
          info << "smart git protocol assumed for repository " << repo_url
               << " due to authentication requirement" <<
            info << "use --git-capabilities to override or suppress this "
                 << "diagnostics";
        }

        // Note that we don't care about the process exit code here and just
        // silently wait for the process completion in the process object
        // destructor. We, however, close the stream (reading out the
        // content), so that the process won't get blocked writing to it.
        //
        // Also note that we drop the potentially redirected process stderr
        // stream content. We even don't read it out, since we assume it fully
        // fits into the pipe buffer.
        //
        is.close ();

        return capabilities::smart;
      }

      // Fail on any other HTTP error (e.g., 404). In the case of a success
      // code other than 200 (e.g. 204 (No Content)) just let the capabilities
      // detection to take its course.
      //
      if (ps.second != 0 && (ps.second < 200 || ps.second >= 300))
      {
        // Note that we don't care about the process exit code here (see above
        // for the reasoning).
        //
        is.close ();

        // Dump the potentially redirected process stderr stream content since
        // it may be helpful to the user.
        //
        // Note, however, that we don't know if it really contains the error
        // description since the fetch program may even exit successfully (see
        // start_fetch_http() for details). Thus, we additionally print the
        // HTTP status code in the diagnostics.
        //
        dump_stderr ();

        fail << "unable to fetch " << url <<
          info << "HTTP status code " << ps.second << endg;
      }

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

      // If the transport is smart let's see if the server also supports
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

      dump_stderr ();

      if (pr.wait ())
        return r;

      // Fall through.
    }
    catch (const io_error&)
    {
      dump_stderr ();

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
      bool pattern (false);

      // If the name is not a valid path, then we don't consider it as a
      // pattern.
      //
      // Note that creating a path starting with '/' (that we use for
      // anchoring search to refs; see below for details) fails on Windows, so
      // we strip it.
      //
      try
      {
        pattern  = path_pattern (path (n[0] != '/'
                                       ? n.c_str ()
                                       : n.c_str () + 1));
      }
      catch (const invalid_path&) {}

      auto search = [this, pattern, &r] (const string& n)
      {
        // Optimize for non-pattern refnames.
        //
        if (pattern)
        {
          path p (n);
          for (const ref& rf: *this)
          {
            if (!rf.peeled && path_match (path (rf.name), p))
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

    // Return the peeled reference for an annotated tag and the original
    // reference if it is not an annotated tag or is already peeled.
    //
    const ref&
    peel (const ref& rf) const
    {
      if (rf.name.compare (0, 10, "refs/tags/") == 0 && !rf.peeled)
      {
        for (const ref& r: *this)
        {
          if (r.peeled && r.name == rf.name)
            return r;
        }

        // Fall through.
        //
        // Presumably is a lightweight tag reference containing the commit id.
        // Note that git-ls-remote output normally contains peeled references
        // for all annotated tags.
        //
      }

      return rf;
    }
  };

  // Map of repository URLs to their advertized refs/commits.
  //
  using repository_refs_map = map<string, refs>;

  static repository_refs_map repository_refs;

  // If the advertized refs/commits are already cached for the specified URL,
  // then return them from the cache. Otherwise, query them and cache. In the
  // latter case, optionally, probe the URL first, calling the specified probe
  // function. Otherwise (the probe function is not specified), it is assumed
  // that the URL has already been probed (sense_capabilities() function was
  // already called for this URL, etc).
  //
  using probe_function = void ();

  static const refs&
  load_refs (const common_options& co,
             const repository_url& url,
             const function<probe_function>& probe = nullptr)
  {
    tracer trace ("load_refs");

    string u (url.string ());
    auto i (repository_refs.find (u));

    if (i != repository_refs.end ())
      return i->second;

    if ((verb && !co.no_progress ()) || co.progress ())
      text << "querying " << url;

    if (probe)
      probe ();

    refs rs;

    for (;;) // Breakout loop.
    {
      fdpipe pipe (open_pipe ());

      // Note: ls-remote doesn't print anything to stderr, so no progress
      // suppression is required.
      //
      process pr (start_git (co,
                             pipe, 2 /* stderr */,
                             timeout_opts (co, url.scheme),
                             co.git_option (),
                             "ls-remote",
                             to_git_url (url)));

      // Shouldn't throw, unless something is severely damaged.
      //
      pipe.out.close ();

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
    auto_fd dev_null (open_null ());

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

          verb < 2 ? "-q" : nullptr,
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
        // Note that the git server communicating with the client using the
        // protocol version 2 always supports unadvertised refs fetch (see the
        // "advertised commit fetch using commit id fails" git bug report for
        // details). We ignore this fact (because currently this is disabled
        // by default, even if both support version 2) but may rely on it in
        // the future.
        //
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
      // Note that url() runs `git config --get remote.origin.url` command on
      // the first call, and so git version get assigned (and checked).
      //
      if (!cap)
      {
        const repository_url& u (url ());

        // Check if the protocol capabilities are overridden for this
        // repository.
        //
        const git_capabilities_map& gcs (co.git_capabilities ());

        if (!gcs.empty () && u.scheme != repository_protocol::file)
        {
          auto i (gcs.find_sup (u.string ()));

          if (i != gcs.end ())
            cap = i->second;
        }

        if (!cap)
          cap = sense_capabilities (co, u, git_ver);
      }

      return *cap;
    };

    function<probe_function> probe ([&caps] () {caps ();});

    auto references = [&co, &url, &probe] (const string& refname,
                                           bool abbr_commit)
      -> refs::search_result
    {
      // Make sure the URL is probed before running git-ls-remote (see
      // load_refs() for details).
      //
      return load_refs (co, url (), probe).search_names (refname, abbr_commit);
    };

    // Return the default reference set (see repository-types(1) for details).
    //
    auto default_references = [&co, &url, &probe] () -> refs::search_result
    {
      // Make sure the URL is probed before running git-ls-remote (see
      // load_refs() for details).
      //
      refs::search_result r;
      vector<standard_version> vs; // Parallel to search_result.

      for (const ref& rf: load_refs (co, url (), probe))
      {
        if (!rf.peeled && rf.name.compare (0, 11, "refs/tags/v") == 0)
        {
          optional<standard_version> v (
            parse_standard_version (string (rf.name, 11),
                                    standard_version::allow_stub));

          if (v)
          {
            // Add this tag reference into the default set if it doesn't
            // contain this version yet or replace the existing reference if
            // this revision is greater.
            //
            auto i (find_if (
                      vs.begin (), vs.end (),
                      [&v] (const standard_version& i)
                      {
                        return i.compare (*v, true /* ignore_revision */) == 0;
                      }));

            if (i == vs.end ())
            {
              r.push_back (rf);
              vs.push_back (move (*v));
            }
            else if (*i < *v)
            {
              r[i - vs.begin ()] = rf;
              *i = move (*v);
            }
          }
        }
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
          // Reduce the reference to the commit id.
          //
          // Note that it is assumed that the URL has already been probed by
          // the above default_references() or references() call.
          //
          const string& c (load_refs (co, url ()).peel (r).commit);

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
      //
      // Check if this is a commit exclusion and remove the corresponding
      // fetch spec if that's the case.
      //
      else if (rf.exclusion)
      {
        remove_spec (*rf.commit);
      }
      //
      // Check if the commit is already fetched and, if that's the case, save
      // it, indicating that no fetch is required.
      //
      else if (commit_fetched (co, dir, *rf.commit))
      {
        add_spec (*rf.commit);
      }
      //
      // If the shallow fetch is possible for the commit, then we fetch it.
      //
      else if (shallow ())
      {
        assert (!rf.exclusion); // Already handled.

        add_spec (*rf.commit, strings ({*rf.commit}), true /* shallow */);
      }
      //
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
      //
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
               << "timestamp" << endg;
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
    auto fetch = [&co, &url, &dir, &caps] (const strings& refspecs,
                                           bool shallow)
    {
      // We don't shallow fetch the whole repository.
      //
      assert (!refspecs.empty () || !shallow);

      // Note that peeled references present in git-ls-remote output are not
      // advertised, so we need to "unpeel" them back to the annotated tag
      // references. Also note that prior to 2.14.0 the git-fetch command
      // didn't accept commit id as a refspec for advertised commits except
      // for servers with dumb or unadv capabilities. Seems that remapping
      // reference ids back to git refs (tags, branches, etc) is harmless and
      // works consistently across different git versions. That's why we will
      // always remap except for the server with the unadv capabilities, where
      // we may not have a name to remap to. That seems OK as no issues with
      // using commit ids for fetching from such servers were encountered so
      // far.
      //
      optional<strings> remapped_refspecs;

      if (!refspecs.empty () && caps () != capabilities::unadv)
      {
        remapped_refspecs = strings ();

        for (const string& c: refspecs)
        {
          const ref* r (load_refs (co, url ()).find_commit (c));
          assert (r != nullptr); // Otherwise we would fail earlier.

          remapped_refspecs->push_back (r->name);
        }
      }

      // If we fetch the whole history, then the --unshallow option is
      // required to make sure that the shallow-fetched branches are also
      // re-fetched. The problem is that git fails if this option is used for
      // a complete repository. A straightforward way to check if our
      // repository is shallow would be using the 'git rev-parse
      // --is-shallow-repository' command. However, the
      // --is-shallow-repository option is not available prior to 2.15.
      // That's why we will check for the .git/shallow file existence,
      // instead.
      //
      auto shallow_repo = [co, &dir] ()
      {
        try
        {
          dir_path d (git_line (co, ".git directory path",
                                co.git_option (),
                                "-C", dir,
                                "rev-parse",
                                "--git-dir"));

          // Resolve the .git directory path if it is relative.
          //
          if (d.relative ())
            d = dir / d;

          return exists (d / "shallow");
        }
        catch (const invalid_path& e)
        {
          fail << "invalid .git directory path '" << e.path << "': " << e
               << endg;
        }
      };

      // Map verbosity level. Suppress the (too detailed) fetch command output
      // if the verbosity level is 1. However, we still want to see the
      // progress in this case, unless we were asked to suppress it (git also
      // suppress progress for a non-terminal stderr).
      //
      cstrings v;
      bool progress (!co.no_progress ());

      if (verb < 2)
      {
        v.push_back ("-q");

        if (progress)
        {
          if ((verb == 1 && stderr_term) || co.progress ())
            v.push_back ("--progress");
        }
        else
          progress = true; // No need to suppress (already done with -q).
      }
      else if (verb > 3)
        v.push_back ("-v");

      // Note that passing --no-tags is not just an optimization. Not doing so
      // we may end up with the "would clobber existing tag" git error for a
      // changed tag (for example, the version tag advanced for revision) if
      // the user has globally configured fetching all remote tags (via the
      // remote.<name>.tagOpt option or similar).
      //
      // Also note that we don't need to specify --refmap option since we can
      // rely on the init() function that properly sets the
      // remote.origin.fetch configuration option.
      //
      if (!run_git (co,
                    progress,
                    timeout_opts (co, url ().scheme),
                    co.git_option (),
                    "-C", dir,
                    "fetch",
                    "--no-tags",
                    "--no-recurse-submodules",
                    (shallow         ? cstrings ({"--depth", "1"}) :
                     shallow_repo () ? cstrings ({"--unshallow"})  :
                     cstrings ()),
                    v,
                    "origin",
                    //
                    // Note that has_value() is used here to work around bogus
                    // "requires the compiler to capture 'this'" VC warning.
                    //
                    (remapped_refspecs.has_value ()
                     ? *remapped_refspecs
                     : refspecs)))
        fail << "unable to fetch " << dir << endg;

      // If we fetched shallow then let's make sure that the method we use to
      // detect if the repository is shallow still works.
      //
      if (shallow && !shallow_repo ())
        fail << "unable to test if " << dir << " is shallow" << endg;
    };

    bool fetch_deep (fetch_repo || !dcs.empty ());

    // Print progress.
    //
    if ((verb && !co.no_progress ()) || co.progress ())
    {
      // Note that the clone command prints the following line prior to the
      // progress lines:
      //
      // Cloning into '<dir>'...
      //
      // The fetch command doesn't print anything similar, for some reason.
      // This makes it hard to understand which superproject/submodule is
      // currently being fetched. Let's fix that.
      //
      // Also note that we have "fixed" that capital letter nonsense and
      // stripped the trailing '...'.
      //
      {
        diag_record dr (text);
        dr << "fetching ";

        if (!submodule.empty ())
          dr << "submodule '" << submodule.posix_string () << "' ";

        dr << "from " << url ();

        if (verb >= 2)
          dr << " in '" << dir.string () << "'"; // Used by tests.
      }

      // Print information messages prior to the deep fetching.
      //
      if (fetch_deep)
      {
        {
          diag_record dr (info);
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
          info << "no progress will be shown (dumb HTTP)";
      }
    }

    // Fetch.
    //
    // First, we perform the deep fetching.
    //
    if (fetch_deep)
    {
      fetch (fetch_repo ? strings () : dcs, false /* shallow */);

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
      fetch (scs, true /* shallow */);

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

  // Print diagnostics, optionally attributing it to a submodule with the
  // specified (non-empty) directory prefix, and fail.
  //
  [[noreturn]] static void
  submodule_failure (const string& desc,
                     const dir_path& prefix,
                     const exception* e = nullptr)
  {
    diag_record dr (fail);
    dr << desc;

    if (!prefix.empty ())
      // Strips the trailing slash.
      //
      dr << " for submodule '" << prefix.string () << "'";

    if (e != nullptr)
      dr << ": " << *e;

    dr << endg;
  }

  // Find submodules for a top repository or submodule directory. The prefix
  // is only used for diagnostics (see submodule_failure() for details).
  //
  struct submodule
  {
    dir_path path; // Relative to the containing repository.
    string name;
    string commit;
  };
  using submodules = vector<submodule>;

  const path gitmodules_file (".gitmodules");

  // If gitmodules is false, then don't check if the .gitmodules file is
  // present, assuming this have already been checked.
  //
  static submodules
  find_submodules (const common_options& co,
                   const dir_path& dir,
                   const dir_path& prefix,
                   bool gitmodules = true)
  {
    tracer trace ("find_submodules");

    submodules r;

    if (gitmodules && !exists (dir / gitmodules_file))
      return r;

    auto failure = [&prefix] (const string& d, const exception* e = nullptr)
    {
      submodule_failure (d, prefix, e);
    };

    // Use git-config to obtain the submodules names/paths and then
    // git-ls-files to obtain their commits.
    //
    // Note that previously we used git-submodule--helper-list subcommand to
    // obtain the submodules commits/paths and then git-submodule--helper-name
    // to obtain their names. However, git 2.38 has removed these subcommands.

    // Obtain the submodules names/paths.
    //
    for (;;) // Breakout loop.
    {
      fdpipe pipe (open_pipe ());

      process pr (start_git (co,
                             pipe, 2 /* stderr */,
                             co.git_option (),
                             "-C", dir,
                             "config",
                             "--list",
                             "--file", gitmodules_file,
                             "-z"));

      // Shouldn't throw, unless something is severely damaged.
      //
      pipe.out.close ();

      try
      {
        ifdstream is (move (pipe.in), fdstream_mode::skip, ifdstream::badbit);

        for (string l; !eof (getline (is, l, '\0')); )
        {
          auto bad = [&l] ()
          {
            throw runtime_error ("invalid submodule option '" + l + '\'');
          };

          // The submodule configuration option line is NULL-terminated and
          // has the following form:
          //
          // submodule.<submodule-name>.<option-name><NEWLINE><value>
          //
          // For example:
          //
          // submodule.style.path
          // doc/style
          //
          l4 ([&]{trace << "submodule option: " << l;});

          // If this is a submodule path option, then extract its name and
          // path and add the entry to the resulting list.
          //
          size_t n (l.find ('\n'));

          if (n != string::npos                    &&
              n >= 15                              &&
              l.compare (0, 10, "submodule.") == 0 &&
              l.compare (n - 5, 5, ".path") == 0)
          {
            string nm (l, 10, n - 15);
            dir_path p (l, n + 1, l.size () - n - 1);

            // For good measure verify that the name and path are not empty.
            //
            if (nm.empty () || p.empty ())
              bad ();

            r.push_back (submodule {move (p), move (nm), empty_string});
          }
        }

        is.close ();

        if (pr.wait ())
          break;

        // Fall through.
      }
      catch (const invalid_path& e)
      {
        if (pr.wait ())
          failure ("invalid submodule directory path '" + e.path + '\'');

        // Fall through.
      }
      catch (const io_error& e)
      {
        if (pr.wait ())
          failure ("unable to read submodule options", &e);

        // Fall through.
      }
      // Note that the io_error class inherits from the runtime_error class,
      // so this catch-clause must go last.
      //
      catch (const runtime_error& e)
      {
        if (pr.wait ())
          failure (e.what ());

        // Fall through.
      }

      // We should only get here if the child exited with an error status.
      //
      assert (!pr.wait ());

      failure ("unable to list submodule options");
    }

    // Note that we could potentially bail out here if the submodules list is
    // empty. Let's however continue and verify that via git-ls-files, for
    // good measure.

    // Complete the resulting submodules information with their commits.
    //
    for (;;) // Breakout loop.
    {
      fdpipe pipe (open_pipe ());

      process pr (start_git (co,
                             pipe, 2 /* stderr */,
                             co.git_option (),
                             "-C", dir,
                             "ls-files",
                             "--stage",
                             "-z"));

      // Shouldn't throw, unless something is severely damaged.
      //
      pipe.out.close ();

      try
      {
        ifdstream is (move (pipe.in), fdstream_mode::skip, ifdstream::badbit);

        for (string l; !eof (getline (is, l, '\0')); )
        {
          auto bad = [&l] ()
          {
            throw runtime_error ("invalid file description '" + l + '\'');
          };

          // The line describing a file is NULL-terminated and has the
          // following form:
          //
          // <mode><SPACE><object><SPACE><stage><TAB><path>
          //
          // The mode is a 6-digit octal representation of the file type and
          // permission bits mask. For a submodule directory it is 160000 (see
          // git index format documentation for gitlink object type). For
          // example:
          //
          // 160000 59dcc1bea3509e37b65905ac472f86f4c55eb510 0	doc/style
          //
          if (!(l.size () > 50 && l[48] == '0' && l[49] == '\t'))
            bad ();

          // For submodules permission bits are always zero, so we can match
          // the mode as a string.
          //
          if (l.compare (0, 6, "160000") == 0)
          {
            l4 ([&]{trace << "submodule: " << l;});

            dir_path d (l, 50, l.size () - 50);

            auto i (find_if (r.begin (), r.end (),
                             [&d] (const submodule& sm) {return sm.path == d;}));

            if (i == r.end ())
              bad ();

            i->commit = string (l, 7, 40);
          }
        }

        is.close ();

        if (pr.wait ())
          break;

        // Fall through.
      }
      catch (const invalid_path& e)
      {
        if (pr.wait ())
          failure ("invalid submodule directory path '" + e.path + '\'');

        // Fall through.
      }
      catch (const io_error& e)
      {
        if (pr.wait ())
          failure ("unable to read repository file list", &e);

        // Fall through.
      }
      // Note that the io_error class inherits from the runtime_error class,
      // so this catch-clause must go last.
      //
      catch (const runtime_error& e)
      {
        if (pr.wait ())
          failure (e.what ());

        // Fall through.
      }

      // We should only get here if the child exited with an error status.
      //
      assert (!pr.wait ());

      failure ("unable to list repository files");
    }

    // Make sure that we have deduced commits for all the submodules.
    //
    for (const submodule& sm: r)
    {
      if (sm.commit.empty ())
        failure ("unable to deduce commit for submodule " + sm.name);
    }

    return r;
  }

  // @@ TMP Old, submodule--helper-{list,name} subcommands-based,
  //    implementation of find_submodules().
  //
#if 0
  static submodules
  find_submodules (const common_options& co,
                   const dir_path& dir,
                   const dir_path& prefix,
                   bool gitmodules = true)
  {
    tracer trace ("find_submodules");

    submodules r;

    if (gitmodules && !exists (dir / gitmodules_file))
      return r;

    auto failure = [&prefix] (const string& d, const exception* e = nullptr)
    {
      submodule_failure (d, prefix, e);
    };

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
          throw runtime_error ("invalid submodule description '" + l + '\'');

        dir_path d (string (l, 50));

        // Note that improper submodule removal (for example, `git rm --cached
        // <submodule-path>` was not executed) can leave git project in the
        // broken state, where the module is not mentioned in .gitmodules but
        // still present in the submodule--helper-list command output. Thus,
        // requesting/returning the module name is not just for the caller's
        // convenience but also for the repository integrity verification.
        //
        string n (git_line (co, "submodule name",
                            co.git_option (),
                            "-C", dir,
                            "submodule--helper", "name",
                            d));

        // Submodule directory path is relative to the containing repository.
        //
        r.push_back (submodule {move (d),
                                move (n),
                                string (l, 7, 40) /* commit */});
      }

      is.close ();

      if (pr.wait ())
        return r;

      // Fall through.
    }
    catch (const invalid_path& e)
    {
      if (pr.wait ())
        failure ("invalid submodule path '" + e.path + '\'');

      // Fall through.
    }
    catch (const io_error& e)
    {
      if (pr.wait ())
        failure ("unable to read submodules list", &e);

      // Fall through.
    }
    // Note that the io_error class inherits from the runtime_error class, so
    // this catch-clause must go last.
    //
    catch (const runtime_error& e)
    {
      if (pr.wait ())
        failure (e.what ());

      // Fall through.
    }

    // We should only get here if the child exited with an error status.
    //
    assert (!pr.wait ());

    submodule_failure ("unable to list submodules", prefix);
  }
#endif

  // Return commit id for the submodule directory or nullopt if the submodule
  // is not initialized (directory doesn't exist, doesn't contain .git entry,
  // etc).
  //
  static optional<string>
  submodule_commit (const common_options& co, const dir_path& dir)
  {
    if (!git_repository (dir))
      return nullopt;

    return git_line (co, "submodule commit",
                     co.git_option (),
                     "-C", dir,
                     "rev-parse",
                     "--verify",
                     "HEAD");
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

    auto failure = [&prefix] (const string& d, const exception* e = nullptr)
    {
      submodule_failure (d, prefix, e);
    };

    path mf (dir / gitmodules_file);

    if (!exists (mf))
      return;

    // Initialize submodules.
    //
    if (!run_git (
          co,
          co.git_option (),
          "-C", dir,

          // Note that git versions outside the [2.14.0 2.38.0) range don't
          // recognize the --super-prefix option but seem to behave correctly
          // without any additional efforts when it is omitted.
          //
          (!prefix.empty ()                       &&
           git_ver >= semantic_version {2, 14, 0} &&
           git_ver <  semantic_version {2, 38, 0}
           ? strings ({"--super-prefix", prefix.posix_representation ()})
           : strings ()),

          "submodule--helper", "init",
          verb < 2 ? "-q" : nullptr))
      failure ("unable to initialize submodules");

    repository_url orig_url (origin_url (co, dir));

    // Iterate over the registered submodules initializing/fetching them and
    // recursively checking them out.
    //
    for (const submodule& sm:
           find_submodules (co, dir, prefix, false /* gitmodules */))
    {
      // Submodule directory path, relative to the top repository.
      //
      dir_path psdir (prefix / sm.path);

      dir_path fsdir (dir / sm.path);     // Submodule full directory path.
      string psd (psdir.posix_string ()); // For use in the diagnostics.

      // The 'none' submodule working tree update method most likely
      // indicates that the submodule is not currently used in the project.
      // Let's skip such submodules as the `git submodule update` command does
      // by default.
      //
      {
        optional<string> u (config_get (co,
                                        mf,
                                        "submodule." + sm.name + ".update",
                                        false /* required */,
                                        "submodule update method"));

        l4 ([&]{trace << "name: " << sm.name << ", "
                      << "update: " << (u ? *u : "[null]");});

        if (u && *u == "none")
        {
          if ((verb >= 2 && !co.no_progress ()) || co.progress ())
            text << "skipping submodule '" << psd << "'";

          // Note that the submodule can be enabled for some other snapshot we
          // are potentially switching from, and so the submodule directory
          // can be non-empty. So let's clean the directory up for good
          // measure.
          //
          if (exists (fsdir))
            rm_r (fsdir, false /* dir_itself */);

          continue;
        }
      }

      string uo ("submodule." + sm.name + ".url");
      string uv (config_get (co, dir, uo, "submodule URL"));

      l4 ([&]{trace << "name: " << sm.name << ", URL: " << uv;});

      // If the submodule is already initialized and its commit didn't
      // change then we skip it.
      //
      optional<string> commit (submodule_commit (co, fsdir));

      if (commit && *commit == sm.commit)
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
          if (commit)
            origin_url (co, fsdir, url);
        }
      }
      catch (const invalid_path& e)
      {
        failure ("invalid submodule '" + sm.name + "' repository path '" +
                 e.path + '\'');
      }
      catch (const invalid_argument& e)
      {
        failure ("invalid submodule '" + sm.name + "' repository URL", &e);
      }

      // Initialize the submodule repository.
      //
      // Note that we initialize the submodule repository git directory out of
      // the working tree, the same way as "submodule--helper clone" does.
      // This prevents us from loosing the fetched data when switching the
      // containing repository between revisions, that potentially contain
      // different sets of submodules.
      //
      dir_path gdir (git_dir / dir_path ("modules") / sm.path);

      if (!commit)
      {
        mk_p (gdir);
        init (co, fsdir, url, gdir);
      }

      // Fetch and checkout the submodule.
      //
      git_ref_filters rfs {
        git_ref_filter {nullopt, sm.commit, false /* exclusion */}};

      fetch (co, fsdir, psdir, rfs);

      git_checkout (co, fsdir, sm.commit);

      // Let's make the message match the git-submodule script output (again,
      // except for capitalization).
      //
      if ((verb && !co.no_progress ()) || co.progress ())
        text << "submodule path '" << psd << "': checked out '" << sm.commit
             << "'";

      // Check out the submodule submodules, recursively.
      //
      checkout_submodules (co, fsdir, gdir, psdir);
    }
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

  // Find symlinks in a working tree of a top repository or submodule
  // (non-recursive submodule-wise) and return their relative paths together
  // with the respective git object ids.
  //
  static vector<pair<path, string>>
  find_symlinks (const common_options& co,
                 const dir_path& dir,
                 const dir_path& prefix)
  {
    tracer trace ("find_symlinks");

    auto failure = [&prefix] (const string& d, const exception* e = nullptr)
    {
      submodule_failure (d, prefix, e);
    };

    fdpipe pipe (open_pipe ());

    // Note: -z tells git to print file paths literally (without escaping) and
    // terminate lines with NUL character.
    //
    process pr (start_git (co,
                           pipe, 2 /* stderr */,
                           co.git_option (),
                           "-C", dir,
                           "ls-files",
                           "--stage",
                           "-z"));

    // Shouldn't throw, unless something is severely damaged.
    //
    pipe.out.close ();

    try
    {
      vector<pair<path, string>> r;
      ifdstream is (move (pipe.in), fdstream_mode::skip, ifdstream::badbit);

      for (string l; !eof (getline (is, l, '\0')); )
      {
        // The line describing a file is NULL-terminated and has the following
        // form:
        //
        // <mode><SPACE><object><SPACE><stage><TAB><path>
        //
        // The mode is a 6-digit octal representation of the file type and
        // permission bits mask. For example:
        //
        // 100644 165b42ec7a10fb6dd4a60b756fa1966c1065ef85 0	README
        //
        if (!(l.size () > 50 && l[48] == '0' && l[49] == '\t'))
          throw runtime_error ("invalid file description '" + l + '\'');

        // For symlinks permission bits are always zero, so we can match the
        // mode as a string.
        //
        if (l.compare (0, 6, "120000") == 0)
        {
          l4 ([&]{trace << "symlink: " << l;});

          r.push_back (make_pair (path (string (l, 50)), string (l, 7, 40)));
        }
      }

      is.close ();

      if (pr.wait ())
        return r;

      // Fall through.
    }
    catch (const invalid_path& e)
    {
      if (pr.wait ())
        failure ("invalid repository symlink path '" + e.path + '\'');

      // Fall through.
    }
    catch (const io_error& e)
    {
      if (pr.wait ())
        failure ("unable to read repository file list", &e);

      // Fall through.
    }
    // Note that the io_error class inherits from the runtime_error class,
    // so this catch-clause must go last.
    //
    catch (const runtime_error& e)
    {
      if (pr.wait ())
        failure (e.what ());

      // Fall through.
    }

    // We should only get here if the child exited with an error status.
    //
    assert (!pr.wait ());

    // Show the noreturn attribute to the compiler to avoid the 'end of
    // non-void function' warning.
    //
    submodule_failure ("unable to list repository files", prefix);
  }

  static void
  git_checkout (const common_options& co,
                const dir_path& dir,
                const string& commit,
#ifdef _WIN32
                const dir_path& prefix)
  {
    // Note that on Windows git may incorrectly deduce the type of a symlink
    // it needs to create. Thus, it is recommended to specify the link type
    // for directory symlinks in the project's .gitattributes file (see the
    // "Using Symlinks in build2 Projects" article for background). However,
    // it turns out that if, for example, such a type has not been specified
    // for some early package version and this have been fixed in some later
    // version, then it may still cause problems even when this later package
    // version is being built. That happens because during the git repository
    // fetch, to produce the available packages list, bpkg sequentially checks
    // out multiple package versions. Git, on the other hand, does not bother
    // re-creating an existing symlink on check out (or git-reset which we
    // use) even though .gitattributes indicates that its type has changed.
    // Thus, on Windows, let's just remove all the existing symlinks prior to
    // running git-reset.
    //
    for (const auto& l: find_symlinks (co, dir, prefix))
    {
      // Note that the symlinks may be filesystem-agnostic (see
      // fixup_worktree() for details) and thus we check the types of the
      // filesystem entries prior to their removal. Also note that the
      // try_rmsymlink() implementation doesn't actually distinguish between
      // the directory and file symlinks and thus we always remove them as the
      // file symlinks.
      //
      path p (dir / l.first);

      pair<bool, entry_stat> e (
        path_entry (p, false /* follow_symlink */, true /* ignore_error */));

      if (e.first && e.second.type == entry_type::symlink)
        try_rmsymlink (p, false /* dir */, true /* ignore_error */);
    }
#else
                const dir_path&)
  {
#endif

    // For some (probably valid) reason the hard reset command doesn't remove
    // a submodule directory that is not plugged into the repository anymore.
    // It also prints the non-suppressible warning like this:
    //
    // warning: unable to rmdir libbar: Directory not empty
    //
    // That's why we run the clean command afterwards. It may also be helpful
    // if we produce any untracked files in the tree between checkouts down
    // the road.
    //
    if (!run_git (co,
                  co.git_option (),
                  "-C", dir,
                  "reset",
                  "--hard",
                  verb < 2 ? "-q" : nullptr,
                  commit))
      fail << "unable to reset to " << commit << endg;

    if (!run_git (co,
                  co.git_option (),
                  "-C", dir,
                  "clean",
                  "-d",
                  "-x",
                  "-ff",
                  verb < 2 ? "-q" : nullptr))
      fail << "unable to clean " << dir << endg;

    // Iterate over the registered submodules and "deinitialize" those whose
    // tip commit has changed.
    //
    // Note that not doing so will make git treat the repository worktree as
    // modified (new commits in submodule). Also the caller may proceed with
    // an inconsistent repository, having no indication that they need to
    // re-run git_checkout_submodules().
    //
    for (const submodule& sm:
           find_submodules (co, dir, dir_path () /* prefix */))
    {
      dir_path sd (dir / sm.path); // Submodule full directory path.

      optional<string> commit (submodule_commit (co, sd));

      // Note that we may re-initialize the submodule later due to the empty
      // directory (see checkout_submodules() for details). Seems that git
      // has no problem with such a re-initialization.
      //
      if (commit && *commit != sm.commit)
        rm_r (sd, false /* dir_itself */);
    }
  }

  void
  git_checkout (const common_options& co,
                const dir_path& dir,
                const string& commit)
  {
    git_checkout (co, dir, commit, dir_path () /* prefix */);
  }

  // Verify symlinks in a working tree of a top repository or submodule,
  // recursively.
  //
  // Specifically, fail if the symlink target is not a valid relative path or
  // refers outside the top repository directory.
  //
  static void
  verify_symlinks (const common_options& co,
                   const dir_path& dir,
                   const dir_path& prefix)
  {
    auto failure = [&prefix] (const string& d, const exception* e = nullptr)
    {
      submodule_failure (d, prefix, e);
    };

    for (const auto& l: find_symlinks (co, dir, prefix))
    {
      const path& lp (l.first);

      // Obtain the symlink target path.
      //
      path tp;

      fdpipe pipe (open_pipe ());
      process pr (start_git (co,
                             pipe, 2 /* stderr */,
                             co.git_option (),
                             "-C", dir,
                             "cat-file",
                             "-p",
                             l.second + "^{object}"));

      // Shouldn't throw, unless something is severely damaged.
      //
      pipe.out.close ();

      try
      {
        ifdstream is (move (pipe.in), fdstream_mode::skip);
        string s (is.read_text ()); // Note: is not newline-terminated.
        is.close ();

        if (pr.wait () && !s.empty ())
        try
        {
          tp = path (move (s));
        }
        catch (const invalid_path& e)
        {
          failure ("invalid target path '" + e.path + "' for symlink '" +
                   lp.string () + '\'',
                   &e);
        }

        // Fall through.
      }
      catch (const io_error&)
      {
        // Fall through.
      }

      if (tp.empty ())
        failure ("unable to read target path for symlink '" + lp.string () +
                 "'");

      // Verify that the symlink target path is relative.
      //
      if (tp.absolute ())
        failure ("absolute target path '" + tp.string () + "' for symlink '" +
                 lp.string () + '\'');

      // Verify that the symlink target path refers inside the top repository
      // directory.
      //
      path rtp (prefix / lp.directory () / tp); // Relative to top directory.
      rtp.normalize (); // Note: can't throw since the path is relative.

      // Normalizing non-empty path can't end up with an empty path.
      //
      assert (!rtp.empty ());

      // Make sure that the relative to the top repository directory target
      // path doesn't start with '..'.
      //
      if (dir_path::traits_type::parent (*rtp.begin ()))
        failure ("target path '" + tp.string () + "' for symlink '" +
                 lp.string () + "' refers outside repository");
    }

    // Verify symlinks for submodules.
    //
    for (const submodule& sm: find_submodules (co, dir, prefix))
      verify_symlinks (co, dir / sm.path, prefix / sm.path);
  }

  void
  git_verify_symlinks (const common_options& co, const dir_path& dir)
  {
    if ((verb && !co.no_progress ()) || co.progress ())
      text << "verifying symlinks...";

    verify_symlinks (co, dir, dir_path () /* prefix */);
  }

#ifndef _WIN32

  // Noop on POSIX.
  //
  optional<bool>
  git_fixup_worktree (const common_options&,
                      const dir_path&,
                      bool revert,
                      bool)
  {
    assert (!revert);
    return false;
  }

#else

  // Fix up or revert the previously made fixes in a working tree of a top
  // repository or submodule (see git_fixup_worktree() description for
  // details). Return nullopt if no changes are required (because real symlink
  // are being used).
  //
  static optional<bool>
  fixup_worktree (const common_options& co,
                  const dir_path& dir,
                  bool revert,
                  const dir_path& prefix)
  {
    bool r (false);

    auto failure = [&prefix] (const string& d, const exception* e = nullptr)
    {
      submodule_failure (d, prefix, e);
    };

    if (!revert)
    {
      // Fix up symlinks depth-first, so link targets in submodules exist by
      // the time we potentially reference them from the containing
      // repository.
      //
      for (const submodule& sm: find_submodules (co, dir, prefix))
      {
        optional<bool> fixed (
          fixup_worktree (co, dir / sm.path, revert, prefix / sm.path));

        // If no further fix up is required, then the repository contains a
        // real symlink. If that's the case, bailout or fail if git's
        // filesystem-agnostic symlinks are also present in the repository.
        //
        if (!fixed)
        {
          // Note that the error message is not precise as path for the
          // symlink in question is no longer available. However, the case
          // feels unusual, so let's not complicate things for now.
          //
          if (r)
            failure ("unexpected real symlink in submodule '" +
                     sm.path.string () + '\'');

          return nullopt;
        }

        if (*fixed)
          r = true;
      }

      // Note that the target belonging to the current repository can be
      // unavailable at the time we create a link to it because its path may
      // contain a not yet created link components. Also, an existing target
      // can be a not yet replaced filesystem-agnostic symlink.
      //
      // First, we cache link/target paths and remove the filesystem-agnostic
      // links from the filesystem in order not to end up hard-linking them as
      // targets. Then, we create links (hardlinks and junctions) iteratively,
      // skipping those with not-yet-existing target, unless no links were
      // created at the previous run, in which case we fail.
      //
      vector<pair<path, string>> ls (find_symlinks (co, dir, prefix));
      vector<pair<path, path>> links; // List of the link/target path pairs.

      // Mark the being replaced in the working tree links as unchanged,
      // running git-update-index(1) for multiple links per run.
      //
      strings unchanged_links; // Links to mark as unchanged.

      auto mark_unchanged = [&unchanged_links, &co, &dir, &failure] ()
      {
        if (!unchanged_links.empty ())
        {
          if (!run_git (co,
                        co.git_option (),
                        "-C", dir,
                        "update-index",
                        "--assume-unchanged",
                        unchanged_links))
            failure ("unable to mark symlinks as unchanged");

          unchanged_links.clear ();
        }
      };

      // Cache/remove filesystem-agnostic symlinks.
      //
      for (auto& li: ls)
      {
        path& l (li.first);
        path lp (dir / l); // Absolute or relative to the current directory.

        // Check the symlink type to see if we need to replace it or can bail
        // out/fail (see above).
        //
        // @@ Note that things are broken here if running in the Windows
        //    "elevated console mode":
        //
        // - file symlinks are currently not supported (see
        //   libbutl/filesystem.hxx for details).
        //
        // - git creates symlinks to directories, rather than junctions. This
        //   makes things to fall apart as Windows API seems to be unable to
        //   see through such directory symlinks. More research is required.
        //
        try
        {
          pair<bool, entry_stat> e (path_entry (lp));

          if (!e.first)
            failure ("symlink '" + l.string () + "' does not exist");

          if (e.second.type == entry_type::symlink)
          {
            if (r)
              failure ("unexpected real symlink '" + l.string () + '\'');

            return nullopt;
          }
        }
        catch (const system_error& e)
        {
          failure ("unable to stat symlink '" + l.string ()  + '\'', &e);
        }

        // Read the symlink target path.
        //
        path t;

        try
        {
          ifdstream fs (lp);
          t = path (fs.read_text ());
        }
        catch (const invalid_path& e)
        {
          failure ("invalid target path '" + e.path + "' for symlink '" +
                   l.string () + '\'',
                   &e);
        }
        catch (const io_error& e)
        {
          failure ("unable to read target path for symlink '" + l.string () +
                   "'",
                   &e);
        }

        // Mark the symlink as unchanged and remove it.
        //
        // Note that we restrict the batch to 100 symlinks not to exceed the
        // Windows command line max size, which is about 32K, and assuming
        // that _MAX_PATH is 256 characters.
        //
        unchanged_links.push_back (l.string ());

        if (unchanged_links.size () == 100)
          mark_unchanged ();

        links.emplace_back (move (l), move (t));

        rm (lp);
        r = true;
      }

      mark_unchanged (); // Mark the rest.

      // Create real links (hardlinks, symlinks, and junctions).
      //
      while (!links.empty ())
      {
        size_t n (links.size ());

        for (auto i (links.cbegin ()); i != links.cend (); )
        {
          const path& l (i->first);
          const path& t (i->second);

          // Absolute or relative to the current directory.
          //
          path lp (dir / l);
          path tp (lp.directory () / t);

          bool dir_target;

          try
          {
            pair<bool, entry_stat> pe (path_entry (tp));

            // Skip the symlink that references a not-yet-existing target.
            //
            if (!pe.first)
            {
              ++i;
              continue;
            }

            dir_target = pe.second.type == entry_type::directory;
          }
          catch (const system_error& e)
          {
            failure ("unable to stat target '" + t.string () +
                     "' for symlink '" + l.string () + '\'',
                     &e);
          }

          // Create the hardlink for a file target and symlink or junction for
          // a directory target.
          //
          try
          {
            if (dir_target)
              mksymlink (t, lp, true /* dir */);
            else
              mkhardlink (tp, lp);
          }
          catch (const system_error& e)
          {
            failure (string ("unable to create ") +
                     (dir_target ? "junction" : "hardlink") + " '" +
                     l.string () + "' with target '" + t.string () + '\'',
                     &e);
          }

          i = links.erase (i);
        }

        // Fail if no links were created on this run.
        //
        if (links.size () == n)
        {
          assert (!links.empty ());

          failure ("target '" + links[0].first.string () + "' for symlink '" +
                   links[0].second.string () + "' does not exist");
        }
      }
    }
    else
    {
      // Revert the fixes we've made previously in the opposite, depth-last,
      // order.
      //
      // For the directory junctions the git-checkout command (see below)
      // removes the target directory content, rather then the junction
      // filesystem entry. To prevent this, we remove all links ourselves
      // first.
      //
      for (const auto& li: find_symlinks (co, dir, prefix))
      {
        const path& l (li.first);

        try
        {
          try_rmfile (dir / l);
        }
        catch (const system_error& e)
        {
          failure ("unable to remove hardlink, symlink, or junction '" +
                   l.string () + '\'',
                   &e);
        }
      }

      if (!run_git (co,
                    co.git_option (),
                    "-C", dir,
                    "checkout",
                    "--",
                    "./"))
        failure ("unable to revert '" + dir.string () + '"');

      // Revert fixes in submodules.
      //
      for (const submodule& sm: find_submodules (co, dir, prefix))
        fixup_worktree (co, dir / sm.path, revert, prefix / sm.path);

      // Let's not complicate things detecting if we have reverted anything
      // and always return true, assuming there wouldn't be a reason to revert
      // if no fixes were made previously.
      //
      r = true;
    }

    return r;
  }

  optional<bool>
  git_fixup_worktree (const common_options& co,
                      const dir_path& dir,
                      bool revert,
                      bool ie)
  {
    if (!revert && ((verb && !co.no_progress ()) || co.progress ()))
      text << "fixing up symlinks...";

    try
    {
      optional<bool> r (
        fixup_worktree (co, dir, revert, dir_path () /* prefix */));

      return r ? *r : false;
    }
    catch (const failed&)
    {
      if (ie)
        return nullopt;

      throw;
    }
  }

#endif
}
