// file      : bpkg/fetch.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch>

#include <fstream>
#include <cstdint> // uint16_t

#include <butl/process>
#include <butl/fdstream>
#include <butl/filesystem>

#include <bpkg/manifest-parser>

#include <bpkg/diagnostics>
#include <bpkg/bpkg-version>

using namespace std;
using namespace butl;

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

    // wget --version prints the version to STDOUT and exits with 0
    // status. The first line starts with "GNU Wget X.Y[.Z].
    //
    const char* args[] = {prog.string ().c_str (), "--version", nullptr};

    if (verb >= 3)
      print_process (args);

    try
    {
      process pr (args, 0, -1); // Redirect STDOUT to a pipe.

      ifdstream is (pr.in_ofd);
      string l;
      getline (is, l);

      if (l.compare (0, 9, "GNU Wget ") != 0)
        return false;

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

        level4 ([&]{trace << "version " << wget_major << '.' << wget_minor;});
      }
      catch (const std::exception&)
      {
        wget_major = 0;
        wget_minor = 0;

        level4 ([&]{trace << "unable to extract version from '" << l << "'";});
      }

      return pr.wait ();
    }
    catch (const process_error& e)
    {
      if (e.child ())
        exit (1);

      return false;
    }
  }

  static process
  start_wget (const path& prog,
              const strings& ops,
              const string& url,
              const path& out)
  {
    bool fo (!out.empty ()); // Output to file.
    string ua (BPKG_USER_AGENT " wget/" + to_string (wget_major) + "."
               + to_string (wget_minor));

    cstrings args {
      prog.string ().c_str (),
      "-U", ua.c_str ()
    };

    // Map verbosity level. If we are running quiet or at level 1
    // and the output is STDOUT, then run wget quiet. If at level
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
      args.push_back ("-q");
    else if (fo && verb == 1)
    {
      // Wget 1.16 introduced the --show-progress option which in the
      // quiet mode shows a nice and tidy progress bar (if only it also
      // showed errors, then it would have been perfect).
      //
      if (wget_major > 1 || (wget_major == 1 && wget_minor >= 16))
      {
        args.push_back ("-q");
        args.push_back ("--show-progress");
      }
    }
    else if (verb > 3)
      args.push_back ("-d");

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

    if (verb >= 2)
      print_process (args);

    // If we are fetching into a file, change the wget's directory to
    // that of the output file. We do it this way so that we end up with
    // just the file name (rather than the whole path) in the progress
    // report. Process exceptions must be handled by the caller.
    //
    return fo
      ? process (out.directory ().string ().c_str (), args.data ())
      : process (args.data (), 0, -1);
  }

  // curl
  //
  static bool
  check_curl (const path& prog)
  {
    // curl --version prints the version to STDOUT and exits with 0
    // status. The first line starts with "curl X.Y.Z"
    //
    const char* args[] = {prog.string ().c_str (), "--version", nullptr};

    if (verb >= 3)
      print_process (args);

    try
    {
      process pr (args, 0, -1); // Redirect STDOUT to a pipe.

      ifdstream is (pr.in_ofd);
      string l;
      getline (is, l);

      return l.compare (0, 5, "curl ") == 0 && pr.wait ();
    }
    catch (const process_error& e)
    {
      if (e.child ())
        exit (1);

      return false;
    }
  }

  static process
  start_curl (const path& prog,
              const strings& ops,
              const string& url,
              const path& out)
  {
    bool fo (!out.empty ()); // Output to file.

    cstrings args {
      prog.string ().c_str (),
      "-f", // Fail on HTTP errors (e.g., 404).
      "-L", // Follow redirects.
      "-A", (BPKG_USER_AGENT " curl")
    };

    // Map verbosity level. If we are running quiet or at level 1
    // and the output is STDOUT, then run curl quiet. If at level
    // 1 and the output is a file, then show the progress bar. At
    // level 2 and 3 run it at the default level (so we will print
    // the command line and it will display its elaborate progress).
    // Higher than that -- run it verbose.
    //
    if (verb < (fo ? 1 : 2))
    {
      args.push_back ("-s");
      args.push_back ("-S"); // But show errors.
    }
    else if (fo && verb == 1)
      args.push_back ("--progress-bar");
    else if (verb > 3)
      args.push_back ("-v");

    // Add extra options. The idea is that they may override what
    // we have set before this point but not after.
    //
    for (const string& o: ops)
      args.push_back (o.c_str ());

    // Output. By default curl writes to STDOUT.
    //
    if (fo)
    {
      args.push_back ("-o");
      args.push_back (out.string ().c_str ());
    }

    args.push_back (url.c_str ());
    args.push_back (nullptr);

    if (verb >= 2)
      print_process (args);
    else if (verb == 1 && fo)
      //
      // Unfortunately curl doesn't print the filename being fetched
      // next to the progress bar. So the best we can do is print it
      // on the previous line. Ugly, I know.
      //
      text << out.leaf () << ':';

    // Process exceptions must be handled by the caller.
    //
    return fo
      ? process (args.data ())
      : process (args.data (), 0, -1);
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

    if (verb >= 3)
      print_process (args);

    try
    {
      process pr (args, 0, -1, 1); // Redirect STDOUT and STDERR to a pipe.

      ifdstream is (pr.in_ofd);
      string l;
      getline (is, l);

      return l.compare (0, 13, "usage: fetch ") == 0;
    }
    catch (const process_error& e)
    {
      if (e.child ())
        exit (1);

      return false;
    }
  }

  static process
  start_fetch (const path& prog,
               const strings& ops,
               const string& url,
               const path& out)
  {
    bool fo (!out.empty ()); // Output to file.

    cstrings args {
      prog.string ().c_str (),
      "--user-agent", (BPKG_USER_AGENT " fetch")
    };

    if (fo)
      args.push_back ("--no-mtime"); // Use our own mtime.

    // Map verbosity level. If we are running quiet then run fetch quiet.
    // If we are at level 1 and we are fetching into a file or we are at
    // level 2 or 3, then run it at the default level (so it will display
    // the progress). Higher than that -- run it verbose.
    //
    if (verb < (fo ? 1 : 2))
      args.push_back ("-q");
    else if (verb > 3)
      args.push_back ("-v");

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

    if (verb >= 2)
      print_process (args);

    // If we are fetching into a file, change the fetch's directory to
    // that of the output file. We do it this way so that we end up with
    // just the file name (rather than the whole path) in the progress
    // report. Process exceptions must be handled by the caller.
    //
    return fo
      ? process (out.directory ().string ().c_str (), args.data ())
      : process (args.data (), 0, -1);
  }

  // The dispatcher.
  //
  // Cache the result of finding/testing the fetch program. Sometimes
  // a simple global variable is really the right solution...
  //
  enum kind {wget, curl, fetch};

  static path fetch_path;
  static kind fetch_kind;

  kind
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

      if (verb > 1)
        info << "using '" << fetch_path << "' as the fetch program, "
             << "use --fetch to override";
    }

    return fetch_kind;
  }

  // If out is empty, then fetch to STDOUT. In this case also don't
  // show any progress unless we are running verbose.
  //
  static process
  start (const common_options& o, const string& url, const path& out = path ())
  {
    process (*f) (
      const path&, const strings&, const string&, const path&) = nullptr;

    switch (check (o))
    {
    case wget:  f = &start_wget;  break;
    case curl:  f = &start_curl;  break;
    case fetch: f = &start_fetch; break;
    }

    try
    {
      return f (fetch_path, o.fetch_option (), url, out);
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << fetch_path << ": " << e.what ();

      if (e.child ())
        exit (1);

      throw failed ();
    }
  }

  static string
  to_url (const string& host, uint16_t port, const path& file)
  {
    //@@ Absolute path in URL: how is this going to work on Windows?
    //   Change to relative: watch for empty path.
    //
    assert (file.absolute ());

    string url ("http://");
    url += host;

    if (port != 0)
      url += ":" + to_string (port);

    url += file.posix_string ();
    return url;
  }

  static path
  fetch_file (const common_options& o,
              const string& host,
              uint16_t port,
              const path& f,
              const dir_path& d)
  {
    path r (d / f.leaf ());

    if (exists (r))
      fail << "file " << r << " already exists";

    string url (to_url (host, port, f));

    auto_rm arm (r);
    process pr (start (o, url, r));

    if (!pr.wait ())
    {
      // While it is reasonable to assuming the child process issued
      // diagnostics, some may not mention the URL.
      //
      fail << "unable to fetch " << url <<
        info << "re-run with -v for more information";
    }

    arm.cancel ();
    return r;
  }

  template <typename M>
  static M
  fetch_manifest (const common_options& o,
                  const string& host,
                  uint16_t port,
                  const path& f)
  {
    string url (to_url (host, port, f));
    process pr (start (o, url));

    try
    {
      ifdstream is (pr.in_ofd);
      is.exceptions (ifdstream::badbit | ifdstream::failbit);

      manifest_parser mp (is, url);
      M m (mp);
      is.close ();

      if (pr.wait ())
        return m;

      // Child existed with an error, fall through.
    }
    // Ignore these exceptions if the child process exited with
    // an error status since that's the source of the failure.
    //
    catch (const manifest_parsing& e)
    {
      if (pr.wait ())
        fail (e.name, e.line, e.column) << e.description;
    }
    catch (const ifdstream::failure&)
    {
      if (pr.wait ())
        fail << "unable to read fetched " << url;
    }

    // We should only get here if the child exited with an error status.
    //
    assert (!pr.wait ());

    // While it is reasonable to assuming the child process issued
    // diagnostics, some may not mention the URL.
    //
    error << "unable to fetch " << url <<
      info << "re-run with -v for more information";
    throw failed ();
  }

  static path
  fetch_file (const path& f, const dir_path& d)
  {
    if (!exists (f))
      fail << "file " << f << " does not exist";

    path r (d / f.leaf ());

    if (exists (r))
      fail << "file " << r << " already exists";

    try
    {
      ifstream ifs (f.string (), ios::binary);
      if (!ifs.is_open ())
        fail << "unable to open " << f << " in read mode";

      auto_rm arm (r);

      ofstream ofs (r.string (), ios::binary);
      if (!ofs.is_open ())
        fail << "unable to open " << r << " in write mode";

      ifs.exceptions (ofstream::badbit | ofstream::failbit);
      ofs.exceptions (ofstream::badbit | ofstream::failbit);

      ofs << ifs.rdbuf();

      // In case they throw.
      //
      ifs.close ();
      ofs.close ();

      arm.cancel ();
    }
    catch (const iostream::failure&)
    {
      fail << "unable to copy " << f << " to " << r << ": io error";
    }

    return r;
  }

  template <typename M>
  static M
  fetch_manifest (const path& f)
  {
    if (!exists (f))
      fail << "file " << f << " does not exist";

    try
    {
      ifstream ifs;
      ifs.exceptions (ofstream::badbit | ofstream::failbit);
      ifs.open (f.string ());

      manifest_parser mp (ifs, f.string ());
      return M (mp);
    }
    catch (const manifest_parsing& e)
    {
      error (e.name, e.line, e.column) << e.description;
    }
    catch (const ifstream::failure&)
    {
      error << "unable to read from " << f;
    }

    throw failed ();
  }

  static const path repositories ("repositories");

  repository_manifests
  fetch_repositories (const dir_path& d)
  {
    return fetch_manifest<repository_manifests> (d / repositories);
  }

  repository_manifests
  fetch_repositories (const common_options& o, const repository_location& rl)
  {
    assert (rl.remote () || rl.absolute ());

    path f (rl.path () / repositories);

    return rl.remote ()
      ? fetch_manifest<repository_manifests> (o, rl.host (), rl.port (), f)
      : fetch_manifest<repository_manifests> (f);
  }

  static const path packages ("packages");

  package_manifests
  fetch_packages (const dir_path& d)
  {
    return fetch_manifest<package_manifests> (d / packages);
  }

  package_manifests
  fetch_packages (const common_options& o, const repository_location& rl)
  {
    assert (rl.remote () || rl.absolute ());

    path f (rl.path () / packages);

    return rl.remote ()
      ? fetch_manifest<package_manifests> (o, rl.host (), rl.port (), f)
      : fetch_manifest<package_manifests> (f);
  }

  path
  fetch_archive (const common_options& o,
                 const repository_location& rl,
                 const path& a,
                 const dir_path& d)
  {
    assert (a.relative ());
    assert (rl.remote () || rl.absolute ());

    path f (rl.path () / a);

    return rl.remote ()
      ? fetch_file (o, rl.host (), rl.port (), f, d)
      : fetch_file (f, d);
  }
}
