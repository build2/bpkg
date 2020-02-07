// file      : bpkg/pkg-unpack.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_UNPACK_HXX
#define BPKG_PKG_UNPACK_HXX

#include <libbpkg/manifest.hxx>     // version
#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package
#include <bpkg/utility.hxx>

#include <bpkg/pkg-unpack-options.hxx>

namespace bpkg
{
  int
  pkg_unpack (const pkg_unpack_options&, cli::scanner& args);

  // Unpack the package as a source directory and commit the transaction. Can
  // return a new selected package object, replacing the existing one.
  //
  shared_ptr<selected_package>
  pkg_unpack (const common_options&,
              const dir_path& configuration,
              transaction&,
              const dir_path&,
              bool replace,
              bool purge,
              bool simulate);

  // Unpack the fetched package and commit the transaction.
  //
  shared_ptr<selected_package>
  pkg_unpack (const common_options&,
              const dir_path& configuration,
              transaction&,
              const package_name&,
              bool simulate);

  // Unpack the package as a source directory from a directory-based
  // repository and commit the transaction. Can return a new selected package
  // object, replacing the existing one.
  //
  shared_ptr<selected_package>
  pkg_unpack (const common_options&,
              const dir_path& configuration,
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
