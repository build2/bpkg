// file      : bpkg/pkg-checkout.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_CHECKOUT_HXX
#define BPKG_PKG_CHECKOUT_HXX

#include <libbpkg/manifest.hxx>     // version
#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package
#include <bpkg/utility.hxx>

#include <bpkg/pkg-checkout-options.hxx>

namespace bpkg
{
  int
  pkg_checkout (const pkg_checkout_options&, cli::scanner& args);

  // Check out the package from a version control-based repository into a
  // directory other than the configuration directory and commit the
  // transaction. Return the selected package object which may replace the
  // existing one.
  //
  shared_ptr<selected_package>
  pkg_checkout (const common_options&,
                database&,
                transaction&,
                package_name,
                version,
                const dir_path& output_root,
                bool replace,
                bool purge,
                bool simulate);

  // Check out the package from a version control-based repository and commit
  // the transaction. Return the selected package object which may replace the
  // existing one.
  //
  shared_ptr<selected_package>
  pkg_checkout (const common_options&,
                database&,
                transaction&,
                package_name,
                version,
                bool replace,
                bool simulate);
}

#endif // BPKG_PKG_CHECKOUT_HXX
