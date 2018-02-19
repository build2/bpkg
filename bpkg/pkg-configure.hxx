// file      : bpkg/pkg-configure.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_CONFIGURE_HXX
#define BPKG_PKG_CONFIGURE_HXX

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>
#include <bpkg/pkg-configure-options.hxx>

namespace bpkg
{
  int
  pkg_configure (const pkg_configure_options&, cli::scanner& args);

  // Configure the package, update its state, and commit the transaction.
  //
  void
  pkg_configure (const dir_path& configuration,
                 const common_options&,
                 transaction&,
                 const shared_ptr<selected_package>&,
                 const strings& config_vars);

  // Configure a system package and commit the transaction.
  //
  shared_ptr<selected_package>
  pkg_configure_system (const string& name, const version&, transaction&);

  // Verify that a directory is a valid package and return its prerequisites.
  // Fail if the directory is not a valid package or some of the prerequisites
  // are not configured or don't satisfy the package's dependency constraints.
  //
  package_prerequisites
  pkg_configure_prerequisites (const common_options&,
                               transaction&,
                               const dir_path& package);
}

#endif // BPKG_PKG_CONFIGURE_HXX
