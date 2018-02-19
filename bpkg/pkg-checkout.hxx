// file      : bpkg/pkg-checkout.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_CHECKOUT_HXX
#define BPKG_PKG_CHECKOUT_HXX

#include <libbpkg/manifest.hxx> // version

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package
#include <bpkg/utility.hxx>

#include <bpkg/pkg-checkout-options.hxx>

namespace bpkg
{
  int
  pkg_checkout (const pkg_checkout_options&, cli::scanner& args);

  // Check out the package from a repository and commit the transaction.
  //
  shared_ptr<selected_package>
  pkg_checkout (const common_options&,
                const dir_path& configuration,
                transaction&,
                string name,
                version,
                bool replace);
}

#endif // BPKG_PKG_CHECKOUT_HXX
