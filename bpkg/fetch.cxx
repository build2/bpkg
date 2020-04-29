// file      : bpkg/fetch.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch.hxx>

#include <bpkg/diagnostics.hxx>

using namespace std;

namespace bpkg
{
  // wget
  //
  static uint16_t wget_major;
  static uint16_t wget_minor;

  static bool
  check_wget (const path& prog)
  {
    tracer trace ("check_wget");

    // wget --version prints the version to stdout and exits with 0
    // status. The first line starts with "GNU Wget X.Y[.Z].
    //
    const char* args[] = {prog.string ().c_str (), "--version", nullptr};

    try
    {
      process_path pp (process::path_search (args[0]));

      if (verb >= 3)
        print_process (args);

      process pr (pp, args, 0, -1); // Redirect stdout to a pipe.

      string l;

      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip);

        getline (is, l);
        is.close ();

        if (!(pr.wait () && l.compare (0, 9, "GNU Wget ") == 0))
          return false;
      }
      catch (const io_error&)
      {
        return false;
      }

      // Extract the version. If something goes wrong, set the version
      // to 0 so that we treat it as a really old wget.
      //
      try
      {
        //l = "GNU Wget 1.8.1";
        string s (l, 9);
        size_t p;
        wget_major = static_cast<uint16_t> (stoul (s, &p));

        if (p != s.size () && s[p] == '.')
          wget_minor = static_cast<uint16_t> (stoul (string (s, p + 1)));

        l4 ([&]{trace << "version " << wget_major << '.' << wget_minor;});
      }
      catch (const std::exception&)
      {
        wget_major = 0;
        wget_minor = 0;

        l4 ([&]{trace << "unable to extract version from '" << l << "'";});
      }

      return true;
    }
    catch (const process_error& e)
    {
      if (e.child)
        exit (1);

      return false;
    }
  }

  static process
  start_wget (const path& prog,
              const optional<size_t>& timeout,
              bool no_progress,
              const strings& ops,
              const string& url,
              const path& out,
              const string& user_agent,
              const string& http_proxy)
  {
    bool fo (!out.empty ()); // Output to file.

    const string& ua (user_agent.empty ()
                      ? BPKG_USER_AGENT " wget/" + to_string (wget_major) +
                        "." + to_string (wget_minor)
                      : user_agent);

    cstrings args {
      prog.string ().c_str (),
      "-U", ua.c_str ()
    };

    // Map verbosity level. If we are running quiet or at level 1
    // and the output is stdout, then run wget quiet. If at level
    // 1 and the output is a file, then show the progress bar. At
    // level 2 and 3 run it at the default level (so we will print
    // the command line and it will display the progress, error
    // messages, etc). Higher than that -- run it with debug output.
    //
    // In the wget world quiet means don't print anything, not even
    // error messages. There is also the -nv mode (aka "non-verbose")
    // which prints error messages but also a useless info-line. So
    // what we are going to do is run it quiet and hope for the best.
    // If things go south, we suggest (in fetch_url()) below that the
    // user re-runs the command with -v to see all the gory details.
    //
    if (verb < (fo ? 1 : 2))
    {
      args.push_back ("-q");
      no_progress = false; // Already suppressed with -q.
    }
    else if (fo && verb == 1)
    {
      // Wget 1.16 introduced the --show-progress option which in the
      // quiet mode shows a nice and tidy progress bar (if only it also
      // showed errors, then it would have been perfect).
      //
      if (wget_major > 1 || (wget_major == 1 && wget_minor >= 16))
      {
        args.push_back ("-q");

        if (!no_progress)
          args.push_back ("--show-progress");
        else
          no_progress = false; // Already suppressed with -q.
      }
    }
    else if (verb > 3)
      args.push_back ("-d");

    // Suppress progress.
    //
    // Note: the `--no-verbose -d` options combination is valid and results in
    // debug messages with the progress meter suppressed.
    //
    if (no_progress)
      args.push_back ("--no-verbose");

    // Set download timeout if requested.
    //
    string tm;
    if (timeout)
    {
      tm = "--timeout=" + to_string (*timeout);
      args.push_back (tm.c_str ());
    }

    // Add extra options. The idea if that they may override what
    // we have set before this point but not after (like -O below).
    //
    for (const string& o: ops)
      args.push_back (o.c_str ());

    // Output.
    //
    string o (fo ? out.leaf ().string () : "-");
    args.push_back ("-O");
    args.push_back (o.c_str ());

    args.push_back (url.c_str ());
    args.push_back (nullptr);

    process_path pp  (process::path_search (args[0]));
    process_env  env (pp);

    // HTTP proxy.
    //
    string evar;
    const char* evars[] = {nullptr, nullptr};

    if (!http_proxy.empty ())
    {
      evar = "http_proxy=" + http_proxy;
      evars[0] = evar.c_str ();
      env.vars = evars;
    }

    if (verb >= 2)
      print_process (env, args);

    // If we are fetching into a file, change the wget's directory to
    // that of the output file. We do it this way so that we end up with
    // just the file name (rather than the whole path) in the progress
    // report. Process exceptions must be handled by the caller.
    //
    return fo
      ? process (pp, args.data (),
                 0, 1, 2,
                 out.directory ().string ().c_str (),
                 env.vars)
      : process (pp, args.data (), 0, -1, 2, nullptr /* cwd */, env.vars);
  }

  // curl
  //
  static bool
  check_curl (const path& prog)
  {
    // curl --version prints the version to stdout and exits with 0
    // status. The first line starts with "curl X.Y.Z"
    //
    const char* args[] = {prog.string ().c_str (), "--version", nullptr};

    try
    {
      process_path pp (process::path_search (args[0]));

      if (verb >= 3)
        print_process (args);

      process pr (pp, args, 0, -1); // Redirect stdout to a pipe.

      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip);

        string l;
        getline (is, l);
        is.close ();

        return pr.wait () && l.compare (0, 5, "curl ") == 0;
      }
      catch (const io_error&)
      {
        // Fall through.
      }
    }
    catch (const process_error& e)
    {
      if (e.child)
        exit (1);

      // Fall through.
    }

    return false;
  }

  static process
  start_curl (const path& prog,
              const optional<size_t>& timeout,
              bool no_progress,
              const strings& ops,
              const string& url,
              const path& out,
              const string& user_agent,
              const string& http_proxy)
  {
    bool fo (!out.empty ()); // Output to file.

    const string& ua (user_agent.empty ()
                      ? string (BPKG_USER_AGENT " curl")
                      : user_agent);

    cstrings args {
      prog.string ().c_str (),
      "-f", // Fail on HTTP errors (e.g., 404).
      "-L", // Follow redirects.
      "-A", ua.c_str ()
    };

    auto suppress_progress = [&args] ()
    {
      args.push_back ("-s");
      args.push_back ("-S"); // But show errors.
    };

    // Map verbosity level. If we are running quiet or at level 1
    // and the output is stdout, then run curl quiet. If at level
    // 1 and the output is a file, then show the progress bar. At
    // level 2 and 3 run it at the default level (so we will print
    // the command line and it will display its elaborate progress).
    // Higher than that -- run it verbose.
    //
    if (verb < (fo ? 1 : 2))
    {
      suppress_progress ();
      no_progress = false; // Already suppressed.
    }
    else if (fo && verb == 1)
    {
      if (!no_progress)
        args.push_back ("--progress-bar");
    }
    else if (verb > 3)
      args.push_back ("-v");

    // Suppress progress.
    //
    // Note: the `-v -s` options combination is valid and results in a verbose
    // output without progress.
    //
    if (no_progress)
      suppress_progress ();

    // Set download timeout if requested.
    //
    string tm;
    if (timeout)
    {
      tm = to_string (*timeout);
      args.push_back ("--max-time");
      args.push_back (tm.c_str ());
    }

    // Add extra options. The idea is that they may override what
    // we have set before this point but not after.
    //
    for (const string& o: ops)
      args.push_back (o.c_str ());

    // Output. By default curl writes to stdout.
    //
    if (fo)
    {
      args.push_back ("-o");
      args.push_back (out.string ().c_str ());
    }

    // HTTP proxy.
    //
    if (!http_proxy.empty ())
    {
      args.push_back ("--proxy");
      args.push_back (http_proxy.c_str ());
    }

    args.push_back (url.c_str ());
    args.push_back (nullptr);

    process_path pp (process::path_search (args[0]));

    if (verb >= 2)
      print_process (args);
    else if (verb == 1 && fo && !no_progress)
      //
      // Unfortunately curl doesn't print the filename being fetched
      // next to the progress bar. So the best we can do is print it
      // on the previous line. Ugly, I know.
      //
      text << out.leaf () << ':';

    // Process exceptions must be handled by the caller.
    //
    return fo
      ? process (pp, args.data ())
      : process (pp, args.data (), 0, -1);
  }

  // fetch
  //
  static bool
  check_fetch (const path& prog)
  {
    // This one doesn't have --version or --help. Running it without
    // any arguments causes it to dump usage and exit with the error
    // status. The usage starts with "usage: fetch " which will be
    // our signature.
    //
    const char* args[] = {prog.string ().c_str (), nullptr};

    try
    {
      process_path pp (process::path_search (args[0]));

      if (verb >= 3)
        print_process (args);

      process pr (pp, args, 0, -1, 1); // Redirect stdout and stderr to a pipe.

      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip);

        string l;
        getline (is, l);
        is.close ();

        return l.compare (0, 13, "usage: fetch ") == 0;
      }
      catch (const io_error&)
      {
        // Fall through.
      }
    }
    catch (const process_error& e)
    {
      if (e.child)
        exit (1);

      // Fall through.
    }

    return false;
  }

  static process
  start_fetch (const path& prog,
               const optional<size_t>& timeout,
               bool no_progress,
               const strings& ops,
               const string& url,
               const path& out,
               const string& user_agent,
               const string& http_proxy)
  {
    bool fo (!out.empty ()); // Output to file.

    const string& ua (user_agent.empty ()
                      ? string (BPKG_USER_AGENT " fetch")
                      : user_agent);

    cstrings args {
      prog.string ().c_str (),
      "--user-agent", ua.c_str ()
    };

    if (fo)
      args.push_back ("--no-mtime"); // Use our own mtime.

    // Map verbosity level. If we are running quiet then run fetch quiet.
    // If we are at level 1 and we are fetching into a file or we are at
    // level 2 or 3, then run it at the default level (so it will display
    // the progress). Higher than that -- run it verbose.
    //
    // Note that the only way to suppress progress for the fetch program is to
    // run it quiet (-q). However, it prints nothing but the progress by
    // default and some additional information in the verbose mode (-v).
    // Therefore, if the progress suppression is requested we will run quiet
    // unless the verbosity level is greater than three, in which case we will
    // run verbose (and with progress). That's the best we can do.
    //
    if (verb < (fo ? 1 : 2))
    {
      args.push_back ("-q");
      no_progress = false;   // Already suppressed with -q.
    }
    else if (verb > 3)
    {
      args.push_back ("-v");
      no_progress = false; // Don't be quiet in the verbose mode (see above).
    }

    // Suppress progress.
    //
    if (no_progress)
      args.push_back ("-q");

    // Set download timeout if requested.
    //
    string tm;
    if (timeout)
    {
      tm = "--timeout=" + to_string (*timeout);
      args.push_back (tm.c_str ());
    }

    // Add extra options. The idea is that they may override what
    // we have set before this point but not after (like -o below).
    //
    for (const string& o: ops)
      args.push_back (o.c_str ());

    // Output.
    //
    string o (fo ? out.leaf ().string () : "-");
    args.push_back ("-o");
    args.push_back (o.c_str ());

    args.push_back (url.c_str ());
    args.push_back (nullptr);

    process_path pp  (process::path_search (args[0]));
    process_env  env (pp);

    // HTTP proxy.
    //
    string evar;
    const char* evars[] = {nullptr, nullptr};

    if (!http_proxy.empty ())
    {
      evar = "HTTP_PROXY=" + http_proxy;
      evars[0] = evar.c_str ();
      env.vars = evars;
    }

    if (verb >= 2)
      print_process (env, args);

    // If we are fetching into a file, change the fetch's directory to
    // that of the output file. We do it this way so that we end up with
    // just the file name (rather than the whole path) in the progress
    // report. Process exceptions must be handled by the caller.
    //
    return fo
      ? process (pp, args.data (),
                 0, 1, 2,
                 out.directory ().string ().c_str (),
                 env.vars)
      : process (pp, args.data (), 0, -1, 2, nullptr /* cwd */, env.vars);
  }

  // The dispatcher.
  //
  // Cache the result of finding/testing the fetch program. Sometimes a simple
  // global variable is really the right solution...
  //
  enum kind {wget, curl, fetch};

  static path fetch_path;
  static kind fetch_kind;

  static kind
  check (const common_options& o)
  {
    if (!fetch_path.empty ())
      return fetch_kind; // Cached.

    if (o.fetch_specified ())
    {
      const path& p (fetch_path = o.fetch ());

      // Figure out which one it is.
      //
      const path& n (p.leaf ());
      const string& s (n.string ());

      if (s.find ("wget") != string::npos)
      {
        if (!check_wget (p))
          fail << p << " does not appear to be the 'wget' program";

        fetch_kind = wget;
      }
      else if (s.find ("curl") != string::npos)
      {
        if (!check_curl (p))
          fail << p << " does not appear to be the 'curl' program";

        fetch_kind = curl;
      }
      else if (s.find ("fetch") != string::npos)
      {
        if (!check_fetch (p))
          fail << p << " does not appear to be the 'fetch' program";

        fetch_kind = fetch;
      }
      else
        fail << "unknown fetch program " << p;
    }
    else
    {
      // See if any is available. The preference order is:
      //
      // wget 1.16 or up
      // curl
      // wget
      // fetch
      //
      bool wg (check_wget (fetch_path = path ("wget")));

      if (wg && (wget_major > 1 || (wget_major == 1 && wget_minor >= 16)))
      {
        fetch_kind = wget;
      }
      else if (check_curl (fetch_path = path ("curl")))
      {
        fetch_kind = curl;
      }
      else if (wg)
      {
        fetch_path = path ("wget");
        fetch_kind = wget;
      }
      else if (check_fetch (fetch_path = path ("fetch")))
      {
        fetch_kind = fetch;
      }
      else
        fail << "unable to find 'wget', 'curl', or 'fetch'" <<
          info << "use --fetch to specify the fetch program location";

      if (verb >= 3)
        info << "using '" << fetch_path << "' as the fetch program, "
             << "use --fetch to override";
    }

    return fetch_kind;
  }

  process
  start_fetch (const common_options& o,
               const string& src,
               const path& out,
               const string& user_agent,
               const url& proxy)
  {
    process (*f) (const path&,
                  const optional<size_t>&,
                  bool,
                  const strings&,
                  const string&,
                  const path&,
                  const string&,
                  const string&) = nullptr;

    switch (check (o))
    {
    case wget:  f = &start_wget;  break;
    case curl:  f = &start_curl;  break;
    case fetch: f = &start_fetch; break;
    }

    optional<size_t> timeout;
    if (o.fetch_timeout_specified ())
      timeout = o.fetch_timeout ();

    // If the HTTP proxy is specified and the URL is HTTP(S), then fetch
    // through the proxy, converting the https URL scheme to http.
    //
    try
    {
      string http_url;
      string http_proxy;

      if (!proxy.empty ())
      {
        auto bad_proxy = [&src, &proxy] (const char* d)
        {
          fail << "unable to fetch '" << src << "' using '" << proxy
               << "' as proxy: " << d;
        };

        if (icasecmp (proxy.scheme, "http") != 0)
          bad_proxy ("only HTTP proxy is supported");

        if (!proxy.authority || proxy.authority->host.empty ())
          bad_proxy ("invalid host name in proxy URL");

        if (!proxy.authority->user.empty ())
          bad_proxy ("unexpected user in proxy URL");

        if (proxy.path)
          bad_proxy ("unexpected path in proxy URL");

        if (proxy.query)
          bad_proxy ("unexpected query in proxy URL");

        if (proxy.fragment)
          bad_proxy ("unexpected fragment in proxy URL");

        if (proxy.rootless)
          bad_proxy ("proxy URL cannot be rootless");

        url u;
        try
        {
          u = url (src);
        }
        catch (const invalid_argument& e)
        {
          fail << "unable to fetch '" << src << "': invalid URL: " << e;
        }

        bool http  (icasecmp (u.scheme, "http")  == 0);
        bool https (icasecmp (u.scheme, "https") == 0);

        if (http || https)
        {
          http_proxy = proxy.string ();

          if (proxy.authority->port == 0)
            http_proxy += ":80";

          if (https)
          {
            u.scheme = "http";
            http_url = u.string ();
          }
        }
      }

      return f (fetch_path,
                timeout,
                o.no_progress (),
                o.fetch_option (),
                !http_url.empty () ? http_url : src,
                out,
                user_agent,
                http_proxy);
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << fetch_path << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }
}
