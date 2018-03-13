// file      : bpkg/fetch-git.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch.hxx>

#ifdef _WIN32
#  include <algorithm> // replace()
#endif

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

    return repository_url (u);
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

  // Return true if a commit is advertised by the remote repository. It is
  // assumed that sense_capabilities() function was already called for the URL.
  //
  static bool
  commit_advertized (const common_options& co,
                     const repository_url& url,
                     const string& commit)
  {
    tracer trace ("commit_advertized");

    fdpipe pipe (open_pipe ());

    process pr (start_git (co,
                           pipe, 2 /* stderr */,
                           timeout_opts (co, url.scheme),
                           co.git_option (),
                           "ls-remote",
                           "--refs",
                           to_git_url (url)));

    pipe.out.close (); // Shouldn't throw, unless something is severely damaged.

    try
    {
      bool r (false);
      ifdstream is (move (pipe.in), fdstream_mode::skip, ifdstream::badbit);

      for (string l; !eof (getline (is, l)); )
      {
        l4 ([&]{trace << "ref: " << l;});

        if (l.compare (0, commit.size (), commit) == 0)
        {
          r = true;
          break;
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
        fail << "unable to read references for " << url << endg;

      // Fall through.
    }

    // We should only get here if the child exited with an error status.
    //
    assert (!pr.wait ());

    fail << "unable to list references for " << url << endg;
  }

  // Return true if the shallow fetch is possible for the reference.
  //
  static bool
  shallow_fetch (const common_options& co,
                 const repository_url& url,
                 capabilities cap,
                 const git_reference& ref)
  {
    switch (cap)
    {
    case capabilities::dumb:
      {
        return false;
      }
    case capabilities::smart:
      {
        return !ref.commit || commit_advertized (co, url, *ref.commit);
      }
    case capabilities::unadv:
      {
        return true;
      }
    }

    assert (false); // Can't be here.
    return false;
  }

  // Return true if a commit is reachable from the tip(s).
  //
  // Can be used to avoid redundant fetches.
  //
  // Note that git-submodule script implements this check, so it is probably an
  // important optimization.
  //
  static bool
  commit_reachable (const common_options& co,
                    const dir_path& dir,
                    const string& commit)
  {
    fdpipe  pipe (open_pipe ());
    auto_fd dev_null (open_dev_null ());

    process pr (start_git (co,
                           pipe,
                           dev_null,
                           co.git_option (),
                           "-C", dir,
                           "rev-list",
                           "-n", "1",
                           commit,
                           "--not",
                           "--all"));

    // Shouldn't throw, unless something is severely damaged.
    //
    pipe.out.close ();
    dev_null.close ();

    try
    {
      ifdstream is (move (pipe.in), fdstream_mode::skip);

      string s;
      if (is.peek () != ifdstream::traits_type::eof ())
        getline (is, s);

      is.close ();
      return pr.wait () && s.empty ();
    }
    catch (const io_error&) {}
    return false;
  }

  // Print warnings about non-shallow fetching.
  //
  static void
  fetch_warn (capabilities cap,
              const char* what,
              const dir_path& submodule = dir_path ())
  {
    {
      diag_record dr (warn);
      dr << "fetching whole " << what << " history";

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

  // Update git index and working tree to match the reference. Fetch if
  // necessary.
  //
  static void
  update_tree (const common_options& co,
               const dir_path& dir,
               const dir_path& submodule, // Is relative to the top project.
               const git_reference& ref,
               capabilities cap,
               bool shallow,
               const strings& to)
  {
    // Don't fetch it the reference is a commit that is reachable from the
    // tip(s).
    //
    if (!(ref.commit && commit_reachable (co, dir, *ref.commit)))
    {
      if (!shallow)
        fetch_warn (cap, ref.commit ? "repository" : "branch", submodule);

      // The clone command prints the following line prior to the progress
      // lines:
      //
      // Cloning into '<dir>'...
      //
      // The fetch command doesn't print anything similar, for some reason.
      // This makes it hard to understand which superproject/submodule is
      // currently being fetched. Let's fix that.
      //
      // Note that we have "fixed" that capital letter nonsense (hoping that
      // git-clone will do the same at some point).
      //
      if (verb != 0)
        text << "fetching in '" << dir.posix_string () << "'...";

      // Note that we suppress the (too detailed) fetch command output if the
      // verbosity level is 1. However, we still want to see the progress in
      // this case, unless STDERR is not directed to a terminal.
      //
      // Also note that we don't need to specify --refmap option since we can
      // rely on the clone command that properly set the remote.origin.fetch
      // configuration option.
      //
      if (!run_git (co,
                    to,
                    co.git_option (),
                    "-C", dir,
                    "fetch",
                    "--no-recurse-submodules",
                    shallow ? cstrings ({"--depth", "1"}) : cstrings (),
                    verb == 1 && fdterm (2) ? opt ( "--progress") : nullopt,
                    verb < 2 ? opt ("-q") : verb > 3 ? opt ("-v") : nullopt,
                    "origin",
                    ref.commit ? *ref.commit : *ref.branch))
        fail << "unable to fetch " << dir << endg;
    }

    const string& commit (ref.commit ? *ref.commit : string ("FETCH_HEAD"));

    // For some (probably valid) reason the hard reset command doesn't remove
    // a submodule directory that is not plugged into the project anymore. It
    // also prints the non-suppressible warning like this:
    //
    // warning: unable to rmdir libbar: Directory not empty
    //
    // That's why we run the clean command afterwards. It may also be helpful
    // if we produce any untracked files in the tree between fetches down the
    // road.
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

  static void
  update_submodules (const common_options& co,
                     const dir_path& dir,
                     const dir_path& prefix)
  {
    tracer trace ("update_submodules");

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
          verb < 1 ? opt ("-q") : nullopt))
      failure ("unable to initialize submodules");

    // Iterate over the registered submodules cloning/fetching them and
    // recursively updating their submodules.
    //
    // Note that we don't expect submodules nesting be too deep and so recurse
    // while reading the git process output.
    //
    fdpipe pipe (open_pipe ());

    process pr (start_git (co,
                           pipe, 2 /* stderr */,
                           co.git_option (),
                           "-C", dir,
                           "submodule--helper", "list"));

    pipe.out.close (); // Shouldn't throw, unless something is severely damaged.

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

        string name (git_string (co, "submodule name",
                                 co.git_option (),
                                 "-C", dir,
                                 "submodule--helper", "name",
                                 sdir));

        repository_url url;

        try
        {
          url = from_git_url (git_string (co, "submodule URL",
                                          co.git_option (),
                                          "-C", dir,
                                          "config",
                                          "--get",
                                          "submodule." + name + ".url"));
        }
        catch (const invalid_argument& e)
        {
          fail << "invalid repository URL for submodule '" << psd << "': "
               << e << endg;
        }

        l4 ([&]{trace << "name: " << name << ", URL: " << url;});

        dir_path fsdir (dir / sdir);
        bool cloned (exists (fsdir / path (".git")));

        // If the submodule is already cloned and it's commit didn't change
        // then we skip it.
        //
        // Note that git-submodule script still recurse into it for some
        // unclear reason.
        //
        if (cloned && git_string (co, "submodule commit",
                                  co.git_option (),
                                  "-C", fsdir,
                                  "rev-parse",
                                  "--verify",
                                  "HEAD") == commit)
          continue;

        git_reference ref {nullopt, commit};
        capabilities cap (sense_capabilities (co, url));
        bool shallow (shallow_fetch (co, url, cap, ref));
        strings to (timeout_opts (co, url.scheme));

        // Clone new submodule.
        //
        if (!cloned)
        {
          if (!shallow)
            fetch_warn (cap, "repository", psdir);

          if (!run_git (co,
                        to,
                        co.git_option (),
                        "-C", dir,
                        "submodule--helper", "clone",

                        "--name", name,
                        "--path", sdir,
                        "--url", to_git_url (url),
                        shallow
                        ? cstrings ({"--depth", "1"})
                        : cstrings (),
                        verb < 1 ? opt ("-q") : nullopt))
            fail << "unable to clone submodule '" << psd << "'" << endg;
        }

        update_tree (co, fsdir, psdir, ref, cap, shallow, to);

        // Not quite a checkout, but let's make the message match the
        // git-submodule script output (again, except for capitalization).
        //
        if (verb > 0)
          text << "submodule path '" << psd << "': checked out '" << commit
               << "'";

        // Recurse.
        //
        // Can throw the failed exception that we don't catch here, relying on
        // the fact that the process destructor will wait for the process
        // completion.
        //
        update_submodules (co, fsdir, psdir);
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

  // Extract the git reference from the repository URL fragment. Set the URL
  // fragment to nullopt.
  //
  static git_reference
  parse_reference (repository_url& url, const char* what)
  {
    try
    {
      git_reference r (git_reference (url.fragment));
      url.fragment = nullopt;
      return r;
    }
    catch (const invalid_argument& e)
    {
      fail << "unable to " << what << ' ' << url << ": " << e << endf;
    }
  }

  // Produce a repository directory name for the specified git reference.
  //
  // Truncate commit id-based directory names to shorten absolute directory
  // paths, lowering the probability of hitting the limit on Windows.
  //
  // Note that we can't truncate them for branches/tags as chances to clash
  // would be way higher than for commit ids. Though such names are normally
  // short anyway.
  //
  static inline dir_path
  repository_dir (const git_reference& ref)
  {
    return dir_path (ref.commit ? ref.commit->substr (0, 16) : *ref.branch);
  }

  dir_path
  git_clone (const common_options& co,
             const repository_location& rl,
             const dir_path& destdir)
  {
    repository_url url (rl.url ());
    git_reference  ref (parse_reference (url, "clone"));

    // All protocols support single branch cloning, so we will always be
    // cloning a single branch if the branch is specified.
    //
    bool single_branch (ref.branch);
    capabilities cap (sense_capabilities (co, url));
    bool shallow (shallow_fetch (co, url, cap, ref));

    if (shallow)
      single_branch = false; // Is implied for shallow cloning.
    else
      fetch_warn (cap, single_branch ? "branch" : "repository");

    dir_path r (repository_dir (ref));
    dir_path d (destdir / r);

    strings to (timeout_opts (co, url.scheme));

    if (!run_git (
          co,
          to,
          "-c", "advice.detachedHead=false",
          co.git_option (),
          "clone",

          ref.branch    ? strings ({"--branch", *ref.branch}) : strings (),
          single_branch ? opt      ("--single-branch")        : nullopt,
          shallow       ? strings ({"--depth", "1"})          : strings (),
          ref.commit    ? opt      ("--no-checkout")          : nullopt,

          verb < 1 ? opt ("-q") : verb > 3 ? opt ("-v") : nullopt,
          to_git_url (url),
          d))
      fail << "unable to clone " << url << endg;

    if (ref.commit)
      update_tree (co, d, dir_path (), ref, cap, shallow, to);

    update_submodules (co, d, dir_path ());
    return r;
  }

  dir_path
  git_fetch (const common_options& co,
             const repository_location& rl,
             const dir_path& destdir)
  {
    repository_url url (rl.url ());
    git_reference  ref (parse_reference (url, "fetch"));

    dir_path r (repository_dir (ref));

    // Fetch is noop if the specific commit is checked out.
    //
    // What if the user replaces the repository URL with a one with a new
    // branch/tag/commit? These are not part of the repository name which
    // means such a repository will have the same hash. But then when we
    // remove the repository, we will also clean up its state. So seems like
    // this should work correctly automatically.
    //
    if (ref.commit)
      return r;

    assert (ref.branch);

    dir_path d (destdir);
    d /= dir_path (*ref.branch);

    // If the repository location differs from the one that was used to clone
    // the repository then we re-clone it from the new location.
    //
    // Another (more hairy) way of doing this would be fixing up the remote
    // origin URLs recursively prior to fetching.
    //
    try
    {
      repository_url u (from_git_url (git_string (co, "remote repository URL",
                                                  co.git_option (),
                                                  "-C", d,
                                                  "config",
                                                  "--get",
                                                  "remote.origin.url")));
      if (u != url)
      {
        // Note that the repository canonical name can not change under the
        // legal scenarios that lead to the location change. Changed canonical
        // name means that the repository was manually amended. We could
        // re-clone such repositories as well but want to leave the backdoor
        // for tests.
        //
        u.fragment = rl.url ().fragment;       // Restore the fragment.
        repository_location l (u, rl.type ());

        if (rl.canonical_name () == l.canonical_name ())
        {
          if (verb)
            info << "re-cloning " << rl.canonical_name ()
                 << " due to location change" <<
              info << "new location " << rl <<
              info << "old location " << l;

          rm_r (d);
          return git_clone (co, rl, destdir);
        }
      }
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid remote.origin.url configuration value: " << e << endg;
    }

    capabilities cap (sense_capabilities (co, url));
    bool shallow (shallow_fetch (co, url, cap, ref));

    update_tree (co,
                 d,
                 dir_path (),
                 ref,
                 cap,
                 shallow,
                 timeout_opts (co, url.scheme));

    update_submodules (co, d, dir_path ());
    return r;
  }
}
