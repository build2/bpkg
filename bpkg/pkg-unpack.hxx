// file      : bpkg/pkg-unpack.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_UNPACK_HXX
#define BPKG_PKG_UNPACK_HXX

#include <libbpkg/manifest.hxx>     // version
#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package, fetch_cache
#include <bpkg/utility.hxx>

#include <bpkg/pkg-unpack-options.hxx>

namespace bpkg
{
  int
  pkg_unpack (const pkg_unpack_options&, cli::scanner& args);

  // Unpack the package as a source directory and commit the transaction.
  // Return the selected package object which may replace the existing one.
  //
  shared_ptr<selected_package>
  pkg_unpack (const common_options&,
              database&,
              transaction&,
              const dir_path&,
              bool replace,
              bool purge,
              bool simulate);

  // Unpack the fetched package and commit the transaction. If the package
  // archive is not used in place, the fetch cache is enabled, and sharing of
  // source directories is not disabled, then the cache should be already open
  // (and this function never closes it), unless in the simulation mode.
  //
  shared_ptr<selected_package>
  pkg_unpack (const common_options&,
              fetch_cache&,
              database&,
              transaction&,
              const package_name&,
              bool simulate);

  // Unpack the package as a source directory from a directory-based
  // repository and commit the transaction. Return the selected package object
  // which may replace the existing one.
  //
  // Note that both package and repository information configurations need to
  // be passed.
  //
  shared_ptr<selected_package>
  pkg_unpack (const common_options&,
              database& pdb,
              database& rdb,
              transaction&,
              package_name,
              version,
              bool replace,
              bool simulate);

  pkg_unpack_options
  merge_options (const default_options<pkg_unpack_options>&,
                 const pkg_unpack_options&);
}

#endif // BPKG_PKG_UNPACK_HXX
