// file      : bpkg/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/utility>

#include <system_error>

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
      return file_exists (d);
    }
    catch (const system_error& e)
    {
      error << "unable to stat path " << d << ": " << e.what ();
      throw failed ();
    }
  }
}
