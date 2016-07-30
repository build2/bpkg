// file      : bpkg/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/utility>

#include <iostream>     // cout, cin

#include <butl/process>

#include <bpkg/diagnostics>
#include <bpkg/common-options>

using namespace std;
using namespace butl;

namespace bpkg
{
  const dir_path bpkg_dir (".bpkg");
  const dir_path certs_dir (dir_path (bpkg_dir) /= "certs");

  bool
  yn_prompt (const char* prompt, char def)
  {
    // Writing a robust Y/N prompt is more difficult than one would
    // expect...
    //
    string a;
    do
    {
      *diag_stream << prompt << ' ';

      // getline() will set the failbit if it failed to extract anything,
      // not even the delimiter and eofbit if it reached eof before seeing
      // the delimiter.
      //
      getline (cin, a);

      bool f (cin.fail ());
      bool e (cin.eof ());

      if (f || e)
        *diag_stream << endl; // Assume no delimiter (newline).

      if (f)
        fail << "unable to read y/n answer from STDOUT";

      if (a.empty () && def != '\0')
      {
        // Don't treat eof as the default answer. We need to see the
        // actual newline.
        //
        if (!e)
          a = def;
      }
    } while (a != "y" && a != "n");

    return a == "y";
  }

  bool
  exists (const path& f)
  {
    try
    {
      return file_exists (f);
    }
    catch (const system_error& e)
    {
      error << "unable to stat path " << f << ": " << e.what ();
      throw failed ();
    }
  }

  bool
  exists (const dir_path& d)
  {
    try
    {
      return dir_exists (d);
    }
    catch (const system_error& e)
    {
      error << "unable to stat path " << d << ": " << e.what ();
      throw failed ();
    }
  }

  bool
  empty (const dir_path& d)
  {
    try
    {
      dir_iterator i (d);
      return i == end (i);
    }
    catch (const system_error& e)
    {
      error << "unable to scan directory " << d << ": " << e.what ();
      throw failed ();
    }
  }

  void
  mk (const dir_path& d)
  {
    if (verb >= 3)
      text << "mkdir " << d;

    try
    {
      try_mkdir (d);
    }
    catch (const system_error& e)
    {
      fail << "unable to create directory " << d << ": " << e.what ();
    }
  }

  void
  mk_p (const dir_path& d)
  {
    if (verb >= 3)
      text << "mkdir -p " << d;

    try
    {
      try_mkdir_p (d);
    }
    catch (const system_error& e)
    {
      fail << "unable to create directory " << d << ": " << e.what ();
    }
  }

  void
  rm (const path& f)
  {
    if (verb >= 3)
      text << "rm " << f;

    try
    {
      if (try_rmfile (f) == rmfile_status::not_exist)
        fail << "unable to remove file " << f << ": file does not exist";
    }
    catch (const system_error& e)
    {
      fail << "unable to remove file " << f << ": " << e.what ();
    }
  }

  void
  rm_r (const dir_path& d, bool dir)
  {
    if (verb >= 3)
      text << "rmdir -r " << d << (dir ? "" : "*");

    try
    {
      rmdir_r (d, dir);
    }
    catch (const system_error& e)
    {
      fail << "unable to remove " << (dir ? "" : "contents of ")
           << "directory " << d << ": " << e.what ();
    }
  }

  void
  run (const char* const args[])
  {
    if (verb >= 2)
      print_process (args);

    try
    {
      process pr (args);

      if (!pr.wait ())
        throw failed (); // Assume the child issued diagnostics.
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e.what ();

      if (e.child ())
        exit (1);

      throw failed ();
    }
  }

  void
  run_b (const common_options& co,
         const dir_path& c,
         const string& bspec,
         bool quiet,
         const strings& pvars,
         const strings& cvars)
  {
    cstrings args {co.build ().string ().c_str ()};

    // Map verbosity level. If we are running quiet or at level 1,
    // then run build2 quiet. Otherwise, run it at the same level
    // as us.
    //
    string vl;
    if (verb <= (quiet ? 1 : 0))
      args.push_back ("-q");
    else if (verb == 2)
      args.push_back ("-v");
    else if (verb > 2)
    {
      vl = to_string (verb);
      args.push_back ("--verbose");
      args.push_back (vl.c_str ());
    }

    // Add user options.
    //
    for (const string& o: co.build_option ())
      args.push_back (o.c_str ());

    // Add config vars.
    //
    strings storage;
    storage.reserve (cvars.size ());
    for (const string& v: cvars)
    {
      // Don't scope-qualify global variables.
      //
      if (v[0] != '!')
      {
        // Use path representation to get canonical trailing slash.
        //
        storage.push_back (c.representation () + ':' + v);
        args.push_back (storage.back ().c_str ());
      }
      else
        args.push_back (v.c_str ());
    }

    for (const string& v: pvars)
      args.push_back (v.c_str ());

    // Add buildspec.
    //
    args.push_back (bspec.c_str ());

    args.push_back (nullptr);
    run (args);
  }

  bool exception_unwinding_dtor = false;
}
