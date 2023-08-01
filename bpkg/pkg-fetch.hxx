// file      : bpkg/pkg-fetch.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_FETCH_HXX
#define BPKG_PKG_FETCH_HXX

#include <libbpkg/manifest.hxx>     // version
#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package
#include <bpkg/utility.hxx>

#include <bpkg/pkg-fetch-options.hxx>

namespace bpkg
{
  int
  pkg_fetch (const pkg_fetch_options&, cli::scanner& args);

  // Fetch the package as an archive file and commit the transaction. Return
  // the selected package object which may replace the existing one.
  //
  shared_ptr<selected_package>
  pkg_fetch (const common_options&,
             database&,
             transaction&,
             path archive,
             bool replace,
             bool purge,
             bool simulate);

  // Fetch the package from an archive-based repository and commit the
  // transaction. Return the selected package object which may replace the
  // existing one.
  //
  // Note that both package and repository information configurations need to
  // be passed.
  //
  // Also note that it should be called in session.
  //
  shared_ptr<selected_package>
  pkg_fetch (const common_options&,
             database& pdb,
             database& rdb,
             transaction&,
             package_name,
             version,
             bool replace,
             bool simulate);

  pkg_fetch_options
  merge_options (const default_options<pkg_fetch_options>&,
                 const pkg_fetch_options&);
}

#endif // BPKG_PKG_FETCH_HXX
