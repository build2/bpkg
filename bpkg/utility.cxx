// file      : bpkg/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/utility>

#include <system_error>

#include <butl/process>
#include <butl/filesystem>

#include <bpkg/types>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
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

  bool exception_unwinding_dtor = false;
}
