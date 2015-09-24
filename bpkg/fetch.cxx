// file      : bpkg/fetch.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch>

#include <fstream>

#include <butl/process>
#include <butl/fdstream>
#include <butl/filesystem>

#include <bpkg/manifest-parser>

#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  template <typename M>
  static M
  fetch_file (const path& f)
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
    return fetch_file<repository_manifests> (d / repositories);
  }

  repository_manifests
  fetch_repositories (const repository_location& rl)
  {
    assert (/*rl.remote () ||*/ rl.absolute ());

    return rl.remote ()
      ? repository_manifests ()
      : fetch_file<repository_manifests> (rl.path () / repositories);
  }

  static const path packages ("packages");

  package_manifests
  fetch_packages (const dir_path& d)
  {
    return fetch_file<package_manifests> (d / packages);
  }

  package_manifests
  fetch_packages (const repository_location& rl)
  {
    assert (/*rl.remote () ||*/ rl.absolute ());

    return rl.remote ()
      ? package_manifests ()
      : fetch_file<package_manifests> (rl.path () / packages);
  }
}
