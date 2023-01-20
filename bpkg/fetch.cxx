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

  // Note that there is no easy way to retrieve the HTTP status code for wget
  // (there is no reliable way to redirect the status line/headers to stdout)
  // and thus we always return 0. Due to the status code unavailability there
  // is no need to redirect stderr and thus we ignore the quiet mode.
  //
  static pair<process, uint16_t>
  start_wget (const path& prog,
              const optional<size_t>& timeout,
              bool progress,
              bool no_progress,
              bool /* quiet */,
              const strings& ops,
              const string& url,
              ifdstream* out_is,
              fdstream_mode out_ism,
              const path& out,
              const string& user_agent,
              const string& http_proxy)
  {
    bool fo (!out.empty ()); // Output to file.

    const string& ua (user_agent.empty ()
                      ? BPKG_USER_AGENT " wget/" + to_string (wget_major) +
                        '.' + to_string (wget_minor)
                      : user_agent);

    cstrings args {
      prog.string ().c_str (),
      "-U", ua.c_str ()
    };

    // Wget 1.16 introduced the --show-progress option which in the quiet mode
    // (-q) shows a nice and tidy progress bar (if only it also showed errors,
    // then it would have been perfect).
    //
    bool has_show_progress (wget_major > 1 ||
                            (wget_major == 1 && wget_minor >= 16));

    // Map verbosity level. If we are running quiet or at level 1
    // and the output is stdout, then run wget quiet. If at level
    // 1 and the output is a file, then show the progress bar. At
    // level 2 and 3 run it at the default level (so we will print
    // the command line and it will display the progress, error
    // messages, etc). Higher than that -- run it with debug output.
    // Always show the progress bar if requested explicitly, even in
    // the quiet mode.
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
      bool quiet (true);

      if (progress)
      {
        // If --show-progress options is supported, then pass both
        // --show-progress and -q, otherwise pass none of them and run
        // verbose.
        //
        if (has_show_progress)
          args.push_back ("--show-progress");
        else
          quiet = false;
      }

      if (quiet)
      {
        args.push_back ("-q");
        no_progress = false; // Already suppressed with -q.
      }
    }
    else if (fo && verb == 1)
    {
      if (has_show_progress)
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
    process pr (fo
                ? process (pp, args.data (),
                           0, 1, 2,
                           out.directory ().string ().c_str (),
                           env.vars)
                : process (pp, args.data (),
                           0, -1, 2,
                           nullptr /* cwd */, env.vars));

    if (out_is != nullptr)
      out_is->open (move (pr.in_ofd), out_ism);

    return make_pair (move (pr), 0);
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

  // If HTTP status code needs to be retrieved, then open the passed stream
  // and read out the status line(s) extracting the status code and the
  // headers. Otherwise, return 0 indicating that the status code is not
  // available. In the former case redirect stderr and respect the quiet mode.
  //
  static pair<process, uint16_t>
  start_curl (const path& prog,
              const optional<size_t>& timeout,
              bool progress,
              bool no_progress,
              bool quiet,
              const strings& ops,
              const string& url,
              ifdstream* out_is,
              fdstream_mode out_ism,
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
    // Higher than that -- run it verbose. Always show the progress
    // bar if requested explicitly, even in the quiet mode.
    //
    if (!quiet)
    {
      if (verb < (fo ? 1 : 2))
      {
        if (!progress)
        {
          suppress_progress ();
          no_progress = false;  // Already suppressed.
        }
      }
      else if (fo && verb == 1)
      {
        if (!no_progress)
          args.push_back ("--progress-bar");
      }
      else if (verb > 3)
        args.push_back ("-v");
    }

    // Suppress progress.
    //
    // Note: the `-v -s` options combination is valid and results in a verbose
    // output without progress.
    //
    if (no_progress || quiet)
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

    // Status code.
    //
    if (out_is != nullptr)
    {
      assert (!fo); // Currently unsupported (see start_fetch() for details).

      args.push_back ("-i");
    }

    args.push_back (url.c_str ());
    args.push_back (nullptr);

    process_path pp (process::path_search (args[0]));

    // Let's still print the command line in the quiet mode to ease the
    // troubleshooting.
    //
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
    process pr (fo
                ? process (pp, args.data ())
                : process (pp, args.data (),
                           0, -1, out_is != nullptr ? -1 : 2));

    // Close the process stdout stream and read stderr stream out and dump.
    //
    // Needs to be called prior to failing, so that the process won't get
    // blocked writing to stdout and so that stderr get dumped before the
    // error message we issue.
    //
    auto close_streams = [&pr, out_is] ()
    {
      try
      {
        out_is->close ();

        bpkg::dump_stderr (move (pr.in_efd));
      }
      catch (const io_error&)
      {
        // Not much we can do here.
      }
    };

    // If HTTP status code needs to be retrieved, then open the passed stream
    // and read out the status line(s) and headers.
    //
    // Note that this implementation is inspired by the bdep's
    // http_service::post() function.
    //
    uint16_t sc (0);

    if (out_is != nullptr)
    try
    {
      // At this stage we will read until the empty line (containing just
      // CRLF). Not being able to reach such a line is an error, which is the
      // reason for the exception mask choice. When done, we will restore the
      // original exception mask.
      //
      ifdstream::iostate es (out_is->exceptions ());

      out_is->exceptions (
        ifdstream::badbit | ifdstream::failbit | ifdstream::eofbit);

      out_is->open (move (pr.in_ofd), out_ism);

      // Parse and return the HTTP status code. Return 0 if the argument is
      // invalid.
      //
      auto status_code = [] (const string& s)
      {
        char* e (nullptr);
        unsigned long c (strtoul (s.c_str (), &e, 10)); // Can't throw.
        assert (e != nullptr);

        return *e == '\0' && c >= 100 && c < 600
        ? static_cast<uint16_t> (c)
        : 0;
      };

      // Read the CRLF-terminated line from the stream stripping the trailing
      // CRLF.
      //
      auto read_line = [out_is] ()
      {
        string l;
        getline (*out_is, l); // Strips the trailing LF (0xA).

        // Note that on POSIX CRLF is not automatically translated into LF, so
        // we need to strip CR (0xD) manually.
        //
        if (!l.empty () && l.back () == '\r')
          l.pop_back ();

        return l;
      };

      auto read_status = [&read_line, &status_code, &url, &close_streams] ()
           -> uint16_t
      {
        string l (read_line ());

        for (;;) // Breakout loop.
        {
          if (l.compare (0, 5, "HTTP/") != 0)
            break;

          size_t p (l.find (' ', 5));           // The protocol end.
          if (p == string::npos)
            break;

          p = l.find_first_not_of (' ', p + 1); // The code start.
          if (p == string::npos)
            break;

          size_t e (l.find (' ', p + 1));       // The code end.
          if (e == string::npos)
            break;

          uint16_t c (status_code (string (l, p, e - p)));
          if (c == 0)
            break;

          return c;
        }

        close_streams ();

        fail << "invalid HTTP response status line '" << l
             << "' while fetching " << url;

        assert (false); // Can't be here.
        return 0;
      };

      sc = read_status ();

      if (sc == 100)
      {
        while (!read_line ().empty ()) ; // Skips the interim response.
        sc = read_status ();             // Reads the final status code.
      }

      while (!read_line ().empty ()) ;   // Skips headers.

      out_is->exceptions (es);
    }
    catch (const io_error&)
    {
      close_streams ();

      fail << "unable to read HTTP response status line for " << url;
    }

    return make_pair (move (pr), sc);
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

  // Note that there is no easy way to retrieve the HTTP status code for the
  // fetch program and thus we always return 0.
  //
  // Also note that in the HTTP status code retrieval mode (out_is != NULL) we
  // nevertheless redirect stderr to prevent the fetch program from
  // interactively querying the user for the credentials. Thus, we also
  // respect the quiet mode in contrast to start_wget().
  //
  static pair<process, uint16_t>
  start_fetch (const path& prog,
               const optional<size_t>& timeout,
               bool progress,
               bool no_progress,
               bool quiet,
               const strings& ops,
               const string& url,
               ifdstream* out_is,
               fdstream_mode out_ism,
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
    // the progress). Higher than that -- run it verbose. Always show the
    // progress bar if requested explicitly, even in the quiet mode.
    //
    // Note that the only way to suppress progress for the fetch program is to
    // run it quiet (-q). However, it prints nothing but the progress by
    // default and some additional information in the verbose mode (-v).
    // Therefore, if the progress suppression is requested we will run quiet
    // unless the verbosity level is greater than three, in which case we will
    // run verbose (and with progress). That's the best we can do.
    //
    if (!quiet)
    {
      if (verb < (fo ? 1 : 2))
      {
        if (!progress)
        {
          args.push_back ("-q");
          no_progress = false;   // Already suppressed with -q.
        }
      }
      else if (verb > 3)
      {
        args.push_back ("-v");
        no_progress = false; // Don't be quiet in the verbose mode (see above).
      }
    }

    // Suppress progress.
    //
    if (no_progress || quiet)
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

    // Let's still print the command line in the quiet mode to ease the
    // troubleshooting.
    //
    if (verb >= 2)
      print_process (env, args);

    // If we are fetching into a file, change the fetch's directory to
    // that of the output file. We do it this way so that we end up with
    // just the file name (rather than the whole path) in the progress
    // report. Process exceptions must be handled by the caller.
    //
    process pr (fo
                ? process (pp, args.data (),
                           0, 1, 2,
                           out.directory ().string ().c_str (),
                           env.vars)
                : process (pp, args.data (),
                           0, -1, out_is != nullptr ? -1 : 2,
                           nullptr /* cwd */, env.vars));

    if (out_is != nullptr)
      out_is->open (move (pr.in_ofd), out_ism);

    return make_pair (move (pr), 0);
  }

  // The dispatcher.
  //
  // Cache the result of finding/testing the fetch program. Sometimes a simple
  // global variable is really the right solution...
  //
  enum class fetch_kind {curl, wget, fetch};

  static path       path_;
  static fetch_kind kind_;

  static fetch_kind
  check (const common_options& o)
  {
    if (!path_.empty ())
      return kind_; // Cached.

    if (o.fetch_specified ())
    {
      const path& p (path_ = o.fetch ());

      // Figure out which one it is.
      //
      const path& n (p.leaf ());
      const string& s (n.string ());

      if (s.find ("curl") != string::npos)
      {
        if (!check_curl (p))
          fail << p << " does not appear to be the 'curl' program";

        kind_ = fetch_kind::curl;
      }
      else if (s.find ("wget") != string::npos)
      {
        if (!check_wget (p))
          fail << p << " does not appear to be the 'wget' program";

        kind_ = fetch_kind::wget;
      }
      else if (s.find ("fetch") != string::npos)
      {
        if (!check_fetch (p))
          fail << p << " does not appear to be the 'fetch' program";

        kind_ = fetch_kind::fetch;
      }
      else
        fail << "unknown fetch program " << p;
    }
    else if (o.curl_specified ())
    {
      const path& p (path_ = o.curl ());

      if (!check_curl (p))
        fail << p << " does not appear to be the 'curl' program";

      kind_ = fetch_kind::curl;
    }
    else
    {
      // See if any is available. The preference order is:
      //
      // curl
      // wget
      // fetch
#if 1
      if (check_curl (path_ = path ("curl")))
      {
        kind_ = fetch_kind::curl;
      }
      else if (check_wget (path_ = path ("wget")))
      {
        kind_ = fetch_kind::wget;
      }
#else
      // Old preference order:
      //
      // wget 1.16 or up
      // curl
      // wget
      // fetch
      //
      // We used to prefer wget 1.16 because it has --show-progress which
      // results in nicer progress. But experience shows that wget is quite
      // unreliable plus with bdep always using curl, it would be strange
      // to use both curl and wget (and expecting the user to setup proxy,
      // authentication, etc., for both).
      //
      bool wg (check_wget (path_ = path ("wget")));

      if (wg && (wget_major > 1 || (wget_major == 1 && wget_minor >= 16)))
      {
        kind_ = fetch_kind::wget;
      }
      else if (check_curl (path_ = path ("curl")))
      {
        kind_ = fetch_kind::curl;
      }
      else if (wg)
      {
        path_ = path ("wget");
        kind_ = fetch_kind::wget;
      }
#endif
      else if (check_fetch (path_ = path ("fetch")))
      {
        kind_ = fetch_kind::fetch;
      }
      else
        fail << "unable to find 'curl', 'wget', or 'fetch'" <<
          info << "use --fetch to specify the fetch program location";

      if (verb >= 3)
        info << "using '" << path_ << "' as the fetch program, "
             << "use --fetch to override";
    }

    return kind_;
  }

  static pair<process, uint16_t>
  start_fetch (const common_options& o,
               const string& src,
               ifdstream* out_is,
               fdstream_mode out_ism,
               bool quiet,
               const path& out,
               const string& user_agent,
               const url& proxy)
  {
    // Currently, for the sake of simplicity, we don't support retrieving the
    // HTTP status code if we fetch into a file.
    //
    assert (out.empty () || out_is == nullptr);

    // Quiet mode is only meaningful if HTTP status code needs to be
    // retrieved.
    //
    assert (!quiet || out_is != nullptr);

    pair<process, uint16_t> (*f) (const path&,
                                  const optional<size_t>&,
                                  bool,
                                  bool,
                                  bool,
                                  const strings&,
                                  const string&,
                                  ifdstream*,
                                  fdstream_mode,
                                  const path&,
                                  const string&,
                                  const string&) = nullptr;

    fetch_kind fk (check (o));
    switch (fk)
    {
    case fetch_kind::curl:  f = &start_curl;  break;
    case fetch_kind::wget:  f = &start_wget;  break;
    case fetch_kind::fetch: f = &start_fetch; break;
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

      // Note that the merge semantics here is not 100% accurate since we may
      // override "later" --fetch-option with "earlier" --curl-option.
      // However, this should be close enough for our use-case, which is
      // bdep's --curl-option values overriding --fetch-option specified in
      // the default options file. The situation that we will mis-handle is
      // when both are specified on the command line, for example,
      // --curl-option --max-time=2 --bpkg-option --fetch-option=--max-time=1,
      // but that feel quite far fetched to complicate things here.
      //
      const strings& fos (o.fetch_option ());
      const strings& cos (o.curl_option ());

      const strings& os (
        fk != fetch_kind::curl || cos.empty ()
        ? fos
        : (fos.empty ()
           ? cos
           : [&fos, &cos] ()
             {
               strings r (fos.begin (), fos.end ());
               r.insert (r.end (), cos.begin (), cos.end ());
               return r;
             } ()));


      return f (path_,
                timeout,
                o.progress (),
                o.no_progress (),
                quiet,
                os,
                !http_url.empty () ? http_url : src,
                out_is,
                out_ism,
                out,
                user_agent,
                http_proxy);
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << path_ << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  process
  start_fetch (const common_options& o,
               const string& src,
               const path& out,
               const string& user_agent,
               const url& proxy)
  {
    return start_fetch (o,
                        src,
                        nullptr /* out_is */,
                        fdstream_mode::none,
                        false /* quiet */,
                        out,
                        user_agent,
                        proxy).first;
  }

  pair<process, uint16_t>
  start_fetch_http (const common_options& o,
                    const string& src,
                    ifdstream& out,
                    fdstream_mode out_mode,
                    bool quiet,
                    const string& user_agent,
                    const url& proxy)
  {
    return start_fetch (o,
                        src,
                        &out,
                        out_mode,
                        quiet,
                        path () /* out */,
                        user_agent,
                        proxy);
  }
}
