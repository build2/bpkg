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

  // Check out the package from a version control-based repository and commit
  // the transaction. Can return a new selected package object, replacing the
  // existing one.
  //
  shared_ptr<selected_package>
  pkg_checkout (const common_options&,
                const dir_path& configuration,
                transaction&,
                package_name,
                version,
                bool replace,
                bool simulate);
}

#endif // BPKG_PKG_CHECKOUT_HXX
