// file      : bpkg/archive.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/archive>

#include <butl/process>
#include <butl/fdstream>

#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  dir_path
  package_dir (const path& a)
  {
    // Strip the top-level extension and, as a special case, if the second-
    // level extension is .tar, strip that as well (e.g., .tar.bz2).
    //
    path d (a.leaf ().base ());
    if (const char* e = d.extension ())
    {
      if (e == string ("tar"))
        d = d.base ();
    }
    return path_cast<dir_path> (d);
  }

  process
  start_extract (const common_options& co,
                 const path& a,
                 const path& f,
                 bool err)
  {
    assert (!f.empty () && f.relative ());

    cstrings args {co.tar ().string ().c_str ()};

    // Add extra options.
    //
    for (const string& o: co.tar_option ())
      args.push_back (o.c_str ());

    // -O/--to-stdout -- extract to STDOUT.
    //
    args.push_back ("-O");

    args.push_back ("-xf");
    args.push_back (a.string ().c_str ());
    args.push_back (f.string ().c_str ());
    args.push_back (nullptr);

    if (verb >= 2)
      print_process (args);

    try
    {
      // If err is false, then redirect STDERR to STDOUT.
      //
      return process (args.data (), 0, -1, (err ? 2 : 1));
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e.what ();

      if (e.child ())
        exit (1);

      throw failed ();
    }
  }

  string
  extract (const common_options& o, const path& a, const path& f)
  try
  {
    process pr (start_extract (o, a, f));

    try
    {
      ifdstream is (pr.in_ofd);

      // Do not throw when eofbit is set (end of stream reached), and
      // when failbit is set (getline() failed to extract any character).
      //
      is.exceptions (ifdstream::badbit);

      string s;
      getline (is, s, '\0');

      if (pr.wait ())
        return s;

      // Fall through.
    }
    catch (const ifdstream::failure&)
    {
      // Child exit status doesn't matter. Just wait for the process
      // completion and fall through.
      //
      pr.wait ();
    }

    // While it is reasonable to assuming the child process issued diagnostics
    // if exited with an error status, tar, specifically, doesn't mention the
    // archive name. So print the error message whatever the child exit status
    // is.
    //
    error << "unable to extract " << f << " from " << a;
    throw failed ();
  }
  catch (const process_error& e)
  {
    error << "unable to extract " << f << " from " << a << ": " << e.what ();
    throw failed ();
  }
}
