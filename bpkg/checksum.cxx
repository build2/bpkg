// file      : bpkg/checksum.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/checksum>

#include <fstream>
#include <streambuf>

#include <butl/process>
#include <butl/fdstream>
#include <butl/filesystem>

#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  // sha256
  //
  static bool
  check_sha256 (const path& prog)
  {
    // This one doesn't have --version or --help. Running it without any
    // arguments causes it to calculate the sum of STDIN. But we can ask
    // it to calculate a sum of an empty string.
    //
    const char* args[] = {prog.string ().c_str (), "-q", "-s", "", nullptr};

    if (verb >= 3)
      print_process (args);

    try
    {
      process pr (args, 0, -1, 1); // Redirect STDOUT and STDERR to a pipe.

      ifdstream is (pr.in_ofd);
      string l;
      getline (is, l);

      return
        l == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        && pr.wait ();
    }
    catch (const process_error& e)
    {
      if (e.child ())
        exit (1);

      return false;
    }
  }

  static process
  start_sha256 (const path& prog, const strings& ops)
  {
    cstrings args {prog.string ().c_str (), "-q"};

    for (const string& o: ops)
      args.push_back (o.c_str ());

    args.push_back (nullptr);

    if (verb >= 2)
      print_process (args);

    // Pipe both STDIN and STDOUT. Process exceptions must be handled by
    // the caller.
    //
    return process (args.data (), -1, -1);
  }

  // sha256sum
  //
  static bool
  check_sha256sum (const path& prog)
  {
    // sha256sum --version prints the version to STDOUT and exits with 0
    // status. The first line starts with "sha256sum (GNU coreutils) 8.21".
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

      return l.compare (0, 9, "sha256sum") == 0 && pr.wait ();
    }
    catch (const process_error& e)
    {
      if (e.child ())
        exit (1);

      return false;
    }
  }

  static process
  start_sha256sum (const path& prog, const strings& ops)
  {
    cstrings args {prog.string ().c_str (), "-b"};

    for (const string& o: ops)
      args.push_back (o.c_str ());

    args.push_back (nullptr);

    if (verb >= 2)
      print_process (args);

    // Pipe both STDIN and STDOUT. Process exceptions must be handled by
    // the caller.
    //
    return process (args.data (), -1, -1);
  }

  // shasum
  //
  static bool
  check_shasum (const path& prog)
  {
    // shasum --version prints just the version to STDOUT and exits with 0
    // status. The output looks like "5.84".
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

      return l.size () != 0 && l[0] >= '0' && l[0] <= '9' && pr.wait ();
    }
    catch (const process_error& e)
    {
      if (e.child ())
        exit (1);

      return false;
    }
  }

  static process
  start_shasum (const path& prog, const strings& ops)
  {
    cstrings args {prog.string ().c_str (), "-a", "256", "-b"};

    for (const string& o: ops)
      args.push_back (o.c_str ());

    args.push_back (nullptr);

    if (verb >= 2)
      print_process (args);

    // Pipe both STDIN and STDOUT. Process exceptions must be handled by
    // the caller.
    //
    return process (args.data (), -1, -1);
  }

  // The dispatcher.
  //
  // Cache the result of finding/testing the sha256 program. Sometimes
  // a simple global variable is really the right solution...
  //
  enum class kind {sha256, sha256sum, shasum};

  static path sha256_path;
  static kind sha256_kind;

  static kind
  check (const common_options& o)
  {
    if (!sha256_path.empty ())
      return sha256_kind; // Cached.

    if (o.sha256_specified ())
    {
      const path& p (sha256_path = o.sha256 ());

      // Figure out which one it is.
      //
      const path& n (p.leaf ());
      const string& s (n.string ());

      if (s.find ("sha256sum") != string::npos)
      {
        if (!check_sha256sum (p))
          fail << p << " does not appear to be the 'sha256sum' program";

        sha256_kind = kind::sha256sum;
      }
      else if (s.find ("shasum") != string::npos)
      {
        if (!check_shasum (p))
          fail << p << " does not appear to be the 'shasum' program";

        sha256_kind = kind::shasum;
      }
      else if (s.find ("sha256") != string::npos)
      {
        if (!check_sha256 (p))
          fail << p << " does not appear to be the 'sha256' program";

        sha256_kind = kind::sha256;
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
      if (check_sha256 (sha256_path = path ("sha256")))
      {
        sha256_kind = kind::sha256;
      }
      else if (check_sha256sum (sha256_path = path ("sha256sum")))
      {
        sha256_kind = kind::sha256sum;
      }
      else if (check_shasum (sha256_path = path ("shasum")))
      {
        sha256_kind = kind::shasum;
      }
      else
        fail << "unable to find 'sha256', 'sha256sum', or 'shasum'" <<
          info << "use --sha256 to specify the sha256 program location";

      if (verb > 1)
        info << "using '" << sha256_path << "' as the sha256 program, "
             << "use --sha256 to override";
    }

    return sha256_kind;
  }

  static process
  start (const common_options& o)
  {
    process (*f) (const path&, const strings&) = nullptr;

    switch (check (o))
    {
    case kind::sha256:    f = &start_sha256;    break;
    case kind::sha256sum: f = &start_sha256sum; break;
    case kind::shasum:    f = &start_shasum;    break;
    }

    try
    {
      process pr (f (sha256_path, o.sha256_option ()));

      // Prevent any data modifications on the way to the hashing program.
      //
      fdmode (pr.out_fd, fdtranslate::binary);
      return pr;
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << sha256_path << ": " << e.what ();

      if (e.child ())
        exit (1);

      throw failed ();
    }
  }

  static string
  sha256 (const common_options& o, streambuf& sb)
  {
    process pr (start (o));

    try
    {
      ofdstream os (pr.out_fd);
      os.exceptions (ofdstream::badbit | ofdstream::failbit);

      ifdstream is (pr.in_ofd);
      is.exceptions (ifdstream::badbit | ifdstream::failbit);

      os << &sb;
      os.close ();

      // All three tools output the sum as the first word.
      //
      string s;
      is >> s;
      is.close ();

      if (pr.wait ())
      {
        if (s.size () != 64)
          fail << "'" << s << "' doesn't appear to be a SHA256 sum" <<
            info << "produced by '" << sha256_path << "'; "
               << "use --sha256 to override";

        return s;
      }

      // Child existed with an error, fall through.
    }
    // Ignore these exceptions if the child process exited with an error status
    // since that's the source of the failure.
    //
    catch (const iostream::failure&)
    {
      if (pr.wait ())
        fail << "unable to read/write '" << sha256_path << "' output/input";
    }

    // We should only get here if the child exited with an error status.
    //
    assert (!pr.wait ());

    // While it is reasonable to assuming the child process issued diagnostics,
    // issue something just in case.
    //
    error << "unable to calculate SHA256 sum using '" << sha256_path << "'" <<
      info << "re-run with -v for more information";

    throw failed ();
  }

  struct memstreambuf: streambuf
  {
    memstreambuf (const char* buf, size_t n)
    {
      char* b (const_cast<char*> (buf));
      setg (b, b, b + n);
    }
  };

  string
  sha256 (const common_options& o, const char* buf, size_t n)
  {
    memstreambuf msb (buf, n);
    return sha256 (o, msb);
  }

  string
  sha256 (const common_options& o, istream& is)
  {
    return sha256 (o, *is.rdbuf ());
  }

  string
  sha256 (const common_options& o, const path& f)
  {
    if (!exists (f))
      fail << "file " << f << " does not exist";

    try
    {
      ifstream ifs (f.string (), ios::binary);
      if (!ifs.is_open ())
        fail << "unable to open " << f << " in read mode";

      ifs.exceptions (ofstream::badbit | ofstream::failbit);

      return sha256 (o, ifs);
    }
    catch (const iostream::failure&)
    {
      error << "unable read " << f;
      throw failed ();
    }
  }
}
