// file      : bpkg/fetch.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch.hxx>

#include <sstream>

#include <libbutl/process.mxx>
#include <libbutl/fdstream.mxx>
#include <libbutl/filesystem.mxx>
#include <libbutl/manifest-parser.mxx>

#include <bpkg/checksum.hxx>
#include <bpkg/diagnostics.hxx>

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

    try
    {
      process_path pp (process::path_search (args[0]));

      if (verb >= 3)
        print_process (args);

      process pr (pp, args, 0, -1); // Redirect STDOUT to a pipe.

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

    process_path pp (process::path_search (args[0]));

    if (verb >= 2)
      print_process (args);

    // If we are fetching into a file, change the wget's directory to
    // that of the output file. We do it this way so that we end up with
    // just the file name (rather than the whole path) in the progress
    // report. Process exceptions must be handled by the caller.
    //
    return fo
      ? process (pp, args.data (),
                 0, 1, 2,
                 out.directory ().string ().c_str ())
      : process (pp, args.data (), 0, -1);
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

    try
    {
      process_path pp (process::path_search (args[0]));

      if (verb >= 3)
        print_process (args);

      process pr (pp, args, 0, -1); // Redirect STDOUT to a pipe.

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

    // Output. By default curl writes to STDOUT.
    //
    if (fo)
    {
      args.push_back ("-o");
      args.push_back (out.string ().c_str ());
    }

    args.push_back (url.c_str ());
    args.push_back (nullptr);

    process_path pp (process::path_search (args[0]));

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

      process pr (pp, args, 0, -1, 1); // Redirect STDOUT and STDERR to a pipe.

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

    process_path pp (process::path_search (args[0]));

    if (verb >= 2)
      print_process (args);

    // If we are fetching into a file, change the fetch's directory to
    // that of the output file. We do it this way so that we end up with
    // just the file name (rather than the whole path) in the progress
    // report. Process exceptions must be handled by the caller.
    //
    return fo
      ? process (pp, args.data (),
                 0, 1, 2,
                 out.directory ().string ().c_str ())
      : process (pp, args.data (), 0, -1);
  }

  // The dispatcher.
  //
  // Cache the result of finding/testing the fetch program. Sometimes
  // a simple global variable is really the right solution...
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

  // If out is empty, then fetch to STDOUT. In this case also don't
  // show any progress unless we are running verbose.
  //
  static process
  start (const common_options& o, const string& url, const path& out = path ())
  {
    process (*f) (const path&,
                  const optional<size_t>&,
                  const strings&,
                  const string&,
                  const path&) = nullptr;

    switch (check (o))
    {
    case wget:  f = &start_wget;  break;
    case curl:  f = &start_curl;  break;
    case fetch: f = &start_fetch; break;
    }

    optional<size_t> timeout;
    if (o.fetch_timeout_specified ())
      timeout = o.fetch_timeout ();

    try
    {
      return f (fetch_path, timeout, o.fetch_option (), url, out);
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << fetch_path << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  static path
  fetch_file (const common_options& o,
              const repository_url& u,
              const dir_path& d)
  {
    path r (d / u.path->leaf ());

    if (exists (r))
      fail << "file " << r << " already exists";

    auto_rmfile arm (r);
    process pr (start (o, u.string (), r));

    if (!pr.wait ())
    {
      // While it is reasonable to assuming the child process issued
      // diagnostics, some may not mention the URL.
      //
      fail << "unable to fetch " << u <<
        info << "re-run with -v for more information";
    }

    arm.cancel ();
    return r;
  }

  template <typename M>
  static pair<M, string/*checksum*/>
  fetch_manifest (const common_options& o,
                  const repository_url& u,
                  bool ignore_unknown)
  {
    string url (u.string ());
    process pr (start (o, url));

    try
    {
      // Unfortunately we cannot read from the original source twice as we do
      // below for files. There doesn't seem to be anything better than reading
      // the entire file into memory and then streaming it twice, once to
      // calculate the checksum and the second time to actually parse. We need
      // to read the original stream in the binary mode for the checksum
      // calculation, then use the binary data to create the text stream for
      // the manifest parsing.
      //
      ifdstream is (move (pr.in_ofd), fdstream_mode::binary);
      stringstream bs (ios::in | ios::out | ios::binary);

      // Note that the eof check is important: if the stream is at eof, write
      // will fail.
      //
      if (is.peek () != ifdstream::traits_type::eof ())
        bs << is.rdbuf ();

      is.close ();

      string s (bs.str ());
      string sha256sum (sha256 (s.c_str (), s.size ()));

      istringstream ts (s); // Text mode.

      manifest_parser mp (ts, url);
      M m (mp, ignore_unknown);

      if (pr.wait ())
        return make_pair (move (m), move (sha256sum));

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
    catch (const io_error&)
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
    fail << "unable to fetch " << url <<
      info << "re-run with -v for more information" << endf;
  }

  static path
  fetch_file (const path& f, const dir_path& d)
  {
    path r (d / f.leaf ());

    try
    {
      cpfile (f, r);
    }
    catch (const system_error& e)
    {
      fail << "unable to copy " << f << " to " << r << ": " << e;
    }

    return r;
  }

  // If o is nullptr, then don't calculate the checksum.
  //
  template <typename M>
  static pair<M, string/*checksum*/>
  fetch_manifest (const common_options* o,
                  const path& f,
                  bool ignore_unknown)
  {
    if (!exists (f))
      fail << "file " << f << " does not exist";

    try
    {
      // We can not use the same file stream for both calculating the checksum
      // and reading the manifest. The file should be opened in the binary
      // mode for the first operation and in the text mode for the second one.
      //
      string sha256sum;
      if (o != nullptr)
        sha256sum = sha256 (*o, f); // Read file in the binary mode.

      ifdstream ifs (f);  // Open file in the text mode.

      manifest_parser mp (ifs, f.string ());
      return make_pair (M (mp, ignore_unknown), move (sha256sum));
    }
    catch (const manifest_parsing& e)
    {
      fail (e.name, e.line, e.column) << e.description << endf;
    }
    catch (const io_error& e)
    {
      fail << "unable to read from " << f << ": " << e << endf;
    }
  }

  static const path repositories ("repositories");

  repository_manifests
  fetch_repositories (const dir_path& d, bool iu)
  {
    return fetch_manifest<repository_manifests> (
      nullptr, d / repositories, iu).first;
  }

  pair<repository_manifests, string/*checksum*/>
  fetch_repositories (const common_options& o,
                      const repository_location& rl,
                      bool iu)
  {
    assert (rl.remote () || rl.absolute ());

    repository_url u (rl.url ());

    path& f (*u.path);
    f /= repositories;

    return rl.remote ()
      ? fetch_manifest<repository_manifests> (o, u, iu)
      : fetch_manifest<repository_manifests> (&o, f, iu);
  }

  static const path packages ("packages");

  package_manifests
  fetch_packages (const dir_path& d, bool iu)
  {
    return fetch_manifest<package_manifests> (nullptr, d / packages, iu).first;
  }

  pair<package_manifests, string/*checksum*/>
  fetch_packages (const common_options& o,
                  const repository_location& rl,
                  bool iu)
  {
    assert (rl.remote () || rl.absolute ());

    repository_url u (rl.url ());

    path& f (*u.path);
    f /= packages;

    return rl.remote ()
      ? fetch_manifest<package_manifests> (o, u, iu)
      : fetch_manifest<package_manifests> (&o, f, iu);
  }

  static const path signature ("signature");

  signature_manifest
  fetch_signature (const common_options& o,
                   const repository_location& rl,
                   bool iu)
  {
    assert (rl.remote () || rl.absolute ());

    repository_url u (rl.url ());

    path& f (*u.path);
    f /= signature;

    return rl.remote ()
      ? fetch_manifest<signature_manifest> (o, u, iu).first
      : fetch_manifest<signature_manifest> (nullptr, f, iu).first;
  }

  path
  fetch_archive (const common_options& o,
                 const repository_location& rl,
                 const path& a,
                 const dir_path& d)
  {
    assert (!a.empty () && a.relative ());
    assert (rl.remote () || rl.absolute ());

    repository_url u (rl.url ());

    path& f (*u.path);
    f /= a;

    auto bad_loc = [&u] () {fail << "invalid archive location " << u;};

    try
    {
      f.normalize ();

      if (*f.begin () == "..") // Can be the case for the remote location.
        bad_loc ();
    }
    catch (const invalid_path&)
    {
      bad_loc ();
    }

    return rl.remote ()
      ? fetch_file (o, u, d)
      : fetch_file (f, d);
  }
}
