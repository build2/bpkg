// file      : bpkg/archive.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/archive.hxx>

#include <libbutl/process.mxx>

#include <bpkg/utility.hxx>
#include <bpkg/diagnostics.hxx>

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
    if (d.extension () == "tar")
      d = d.base ();
    return path_cast<dir_path> (d);
  }

  pair<process, process>
  start_extract (const common_options& co,
                 const path& a,
                 const path& f,
                 bool err)
  {
    assert (!f.empty () && f.relative ());

    cstrings args;

    // See if we need to decompress.
    //
    {
      string e (a.extension ());

      if      (e == "gz")    args.push_back ("gzip");
      else if (e == "bzip2") args.push_back ("bzip2");
      else if (e == "xz")    args.push_back ("xz");
      else if (e != "tar")
        fail << "unknown compression method in " << a;
    }

    size_t i (0); // The tar command line start.
    if (!args.empty ())
    {
      args.push_back ("-dc");
      args.push_back (a.string ().c_str ());
      args.push_back (nullptr);
      i = args.size ();
    }

    args.push_back (co.tar ().string ().c_str ());

    // Add extra options.
    //
    for (const string& o: co.tar_option ())
      args.push_back (o.c_str ());

    // -O/--to-stdout -- extract to stdout.
    //
    args.push_back ("-O");

    // An archive name that has a colon in it specifies a file or device on a
    // remote machine. That makes it impossible to use absolute Windows paths
    // unless we add the --force-local option. Note that BSD tar doesn't
    // support this option.
    //
#ifdef _WIN32
    args.push_back ("--force-local");
#endif

    args.push_back ("-xf");
    args.push_back (i == 0 ? a.string ().c_str () : "-");

    // MSYS tar doesn't find archived file if it's path is provided in Windows
    // notation.
    //
    string fs (f.posix_string ());
    args.push_back (fs.c_str ());

    args.push_back (nullptr);
    args.push_back (nullptr); // Pipe end.

    size_t what;
    try
    {
      process_path dpp;
      process_path tpp;

      process dpr;
      process tpr;

      if (i != 0)
        dpp = process::path_search (args[what = 0]);

      tpp = process::path_search (args[what = i]);

      if (verb >= 2)
        print_process (args);

      // If err is false, then redirect stderr to stdout.
      //
      auto_fd nfd (err ? nullfd : fdnull ());

      if (i != 0)
      {
        dpr = process (dpp, &args[what = 0], 0,   -1, (err ? 2 : nfd.get ()));
        tpr = process (tpp, &args[what = i], dpr, -1, (err ? 2 : nfd.get ()));
      }
      else
        tpr = process (tpp, &args[what = 0], 0, -1, (err ? 2 : nfd.get ()));

      return make_pair (move (dpr), move (tpr));
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[what] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  string
  extract (const common_options& o, const path& a, const path& f)
  try
  {
    pair<process, process> pr (start_extract (o, a, f));

    try
    {
      // Do not throw when eofbit is set (end of stream reached), and
      // when failbit is set (getline() failed to extract any character).
      //
      ifdstream is (move (pr.second.in_ofd), ifdstream::badbit);

      string s (is.read_text ());
      is.close ();

      if (pr.second.wait () && pr.first.wait ())
        return s;

      // Fall through.
    }
    catch (const io_error&)
    {
      // Child exit status doesn't matter. Just wait for the process
      // completion and fall through.
      //
      pr.second.wait (); pr.first.wait (); // Check throw.
    }

    // While it is reasonable to assuming the child process issued diagnostics
    // if exited with an error status, tar, specifically, doesn't mention the
    // archive name. So print the error message whatever the child exit status
    // is.
    //
    fail << "unable to extract " << f << " from " << a << endf;
  }
  catch (const process_error& e)
  {
    fail << "unable to extract " << f << " from " << a << ": " << e << endf;
  }
}
