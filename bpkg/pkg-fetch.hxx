// file      : bpkg/pkg-fetch.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_FETCH_HXX
#define BPKG_PKG_FETCH_HXX

#include <libbpkg/manifest.hxx> // version

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package
#include <bpkg/utility.hxx>

#include <bpkg/pkg-fetch-options.hxx>

namespace bpkg
{
  int
  pkg_fetch (const pkg_fetch_options&, cli::scanner& args);

  // Fetch the package as an archive file and commit the transaction.
  //
  shared_ptr<selected_package>
  pkg_fetch (const common_options&,
             const dir_path& configuration,
             transaction&,
             path archive,
             bool replace,
             bool purge);

  // Fetch the package from an archive-based repository and commit the
  // transaction.
  //
  shared_ptr<selected_package>
  pkg_fetch (const common_options&,
             const dir_path& configuration,
             transaction&,
             string name,
             version,
             bool replace);
}

#endif // BPKG_PKG_FETCH_HXX
