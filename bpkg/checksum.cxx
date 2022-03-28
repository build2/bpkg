// file      : bpkg/checksum.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/checksum.hxx>

#include <bpkg/diagnostics.hxx>

using namespace std;

namespace bpkg
{
  // sha256
  //
  static bool
  check_sha256 (const path& prog)
  {
    // This one doesn't have --version or --help. Running it without any
    // arguments causes it to calculate the sum of stdin. But we can ask
    // it to calculate a sum of an empty string.
    //
    const char* args[] = {prog.string ().c_str (), "-q", "-s", "", nullptr};

    try
    {
      process_path pp (process::path_search (args[0]));

      if (verb >= 3)
        print_process (args);

      process pr (pp, args, 0, -1, 1); // Redirect stdout and stderr to a pipe.

      try
      {
        ifdstream is (move (pr.in_ofd));

        string l;
        getline (is, l);
        is.close ();

        return
          pr.wait () &&
          l ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
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
  start_sha256 (const path& prog, const strings& ops, const path& file)
  {
    cstrings args {prog.string ().c_str (), "-q"};

    for (const string& o: ops)
      args.push_back (o.c_str ());

    args.push_back (file.string ().c_str ());
    args.push_back (nullptr);

    process_path pp (process::path_search (args[0]));

    if (verb >= 3)
      print_process (args);

    // Pipe stdout. Process exceptions must be handled by the caller.
    //
    return process (pp, args.data (), 0, -1);
  }

  // sha256sum
  //
  static bool
  check_sha256sum (const path& prog)
  {
    // sha256sum --version prints the version to stdout and exits with 0
    // status. The first line starts with "sha256sum (GNU coreutils) 8.21".
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

        return pr.wait () && l.compare (0, 9, "sha256sum") == 0;
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
  start_sha256sum (const path& prog, const strings& ops, const path& file)
  {
    cstrings args {prog.string ().c_str (), "-b"};

    for (const string& o: ops)
      args.push_back (o.c_str ());

    // For some reason, MSYS2-based sha256sum utility prints stray backslash
    // character at the beginning of the sum if the path contains a
    // backslash. So we get rid of them.
    //
#ifndef _WIN32
    const string& f (file.string ());
#else
    string f (file.string ());
    replace (f.begin (), f.end (), '\\', '/');
#endif

    args.push_back (f.c_str ());
    args.push_back (nullptr);

    process_path pp (process::path_search (args[0]));

    if (verb >= 3)
      print_process (args);

    // Pipe stdout. Process exceptions must be handled by the caller.
    //
    return process (pp, args.data (), 0, -1);
  }

  // shasum
  //
  static bool
  check_shasum (const path& prog)
  {
    // shasum --version prints just the version to stdout and exits with 0
    // status. The output looks like "5.84".
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
        ifdstream is (move (pr.in_ofd));

        string l;
        getline (is, l);
        is.close ();

        return pr.wait () && l.size () != 0 && l[0] >= '0' && l[0] <= '9';
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
  start_shasum (const path& prog, const strings& ops, const path& file)
  {
    cstrings args {prog.string ().c_str (), "-a", "256", "-b"};

    for (const string& o: ops)
      args.push_back (o.c_str ());

    args.push_back (file.string ().c_str ());
    args.push_back (nullptr);

    process_path pp (process::path_search (args[0]));

    if (verb >= 3)
      print_process (args);

    // Pipe stdout. Process exceptions must be handled by the caller.
    //
    return process (pp, args.data (), 0, -1);
  }

  // The dispatcher.
  //
  // Cache the result of finding/testing the sha256 program. Sometimes
  // a simple global variable is really the right solution...
  //
  enum class sha256_kind {sha256, sha256sum, shasum};

  static path        path_;
  static sha256_kind kind_;

  static sha256_kind
  check (const common_options& o)
  {
    if (!path_.empty ())
      return kind_; // Cached.

    if (o.sha256_specified ())
    {
      const path& p (path_ = o.sha256 ());

      // Figure out which one it is.
      //
      const path& n (p.leaf ());
      const string& s (n.string ());

      if (s.find ("sha256sum") != string::npos)
      {
        if (!check_sha256sum (p))
          fail << p << " does not appear to be the 'sha256sum' program";

        kind_ = sha256_kind::sha256sum;
      }
      else if (s.find ("shasum") != string::npos)
      {
        if (!check_shasum (p))
          fail << p << " does not appear to be the 'shasum' program";

        kind_ = sha256_kind::shasum;
      }
      else if (s.find ("sha256") != string::npos)
      {
        if (!check_sha256 (p))
          fail << p << " does not appear to be the 'sha256' program";

        kind_ = sha256_kind::sha256;
      }
      else
        fail << "unknown sha256 program " << p;
    }
    else
    {
      // See if any is available. The preference order is:
      //
      // sha256    (FreeBSD)
      // sha256sum (Linux coreutils)
      // shasum    (Perl tool, Mac OS)
      //
      if (check_sha256 (path_ = path ("sha256")))
      {
        kind_ = sha256_kind::sha256;
      }
      else if (check_sha256sum (path_ = path ("sha256sum")))
      {
        kind_ = sha256_kind::sha256sum;
      }
      else if (check_shasum (path_ = path ("shasum")))
      {
        kind_ = sha256_kind::shasum;
      }
      else
        fail << "unable to find 'sha256', 'sha256sum', or 'shasum'" <<
          info << "use --sha256 to specify the sha256 program location";

      if (verb >= 3)
        info << "using '" << path_ << "' as the sha256 program, "
             << "use --sha256 to override";
    }

    return kind_;
  }

  static process
  start (const common_options& o, const path& f)
  {
    process (*sf) (const path&, const strings&, const path&) = nullptr;

    switch (check (o))
    {
    case sha256_kind::sha256:    sf = &start_sha256;    break;
    case sha256_kind::sha256sum: sf = &start_sha256sum; break;
    case sha256_kind::shasum:    sf = &start_shasum;    break;
    }

    try
    {
      return sf (path_, o.sha256_option (), f);
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << path_ << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  string
  sha256sum (const common_options& o, const path& f)
  {
    if (!exists (f))
      fail << "file " << f << " does not exist";

    process pr (start (o, f));

    try
    {
      ifdstream is (move (pr.in_ofd), fdstream_mode::skip);

      // All three tools output the sum as the first word.
      //
      string s;
      is >> s;
      is.close ();

      if (pr.wait ())
      {
        if (s.size () != 64)
          fail << "'" << s << "' doesn't appear to be a SHA256 sum" <<
            info << "produced by '" << path_ << "'; use --sha256 to override";

        return s;
      }

      // Child exited with an error, fall through.
    }
    // Ignore these exceptions if the child process exited with an error status
    // since that's the source of the failure.
    //
    catch (const io_error&)
    {
      if (pr.wait ())
        fail << "unable to read '" << path_ << "' output";
    }

    // We should only get here if the child exited with an error status.
    //
    assert (!pr.wait ());

    // While it is reasonable to assuming the child process issued diagnostics,
    // issue something just in case.
    //
    fail << "unable to calculate SHA256 sum using '" << path_ << "'" <<
      info << "re-run with -v for more information" << endf;
  }
}
