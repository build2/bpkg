// file      : bpkg/pkg-configure.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_CONFIGURE_HXX
#define BPKG_PKG_CONFIGURE_HXX

#include <libbpkg/manifest.hxx>     // version
#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>               // package_prerequisites,
                                          // dependencies.
#include <bpkg/pkg-configure-options.hxx>

namespace bpkg
{
  int
  pkg_configure (const pkg_configure_options&, cli::scanner& args);

  // The custom search function, if specified, is called by pkg_configure()
  // and pkg_configure_prerequisites() to obtain the database to search for
  // the prerequisite in, instead of searching for it in the associated
  // databases, recursively. If the function returns NULL, then fallback to
  // the recursive search through the associated databases.
  //
  using find_prereq_database_function = database* (database&,
                                                   const package_name&,
                                                   bool buildtime);

  // Note: all of the following functions expect the package dependency
  // constraints to be complete.

  // Configure the package, update its state, and commit the transaction.
  //
  void
  pkg_configure (const common_options&,
                 database&,
                 transaction&,
                 const shared_ptr<selected_package>&,
                 const dependencies&,
                 const strings& config_vars,
                 bool simulate,
                 const function<find_prereq_database_function>& = {});

  // Configure a system package and commit the transaction.
  //
  shared_ptr<selected_package>
  pkg_configure_system (const package_name&,
                        const version&,
                        database&,
                        transaction&);

  // Return package prerequisites given its dependencies. Fail if some of the
  // prerequisites are not configured or don't satisfy the package's
  // dependency constraints. Note that the package argument is used for
  // diagnostics only.
  //
  // Note: loads selected packages.
  //
  package_prerequisites
  pkg_configure_prerequisites (
    const common_options&,
    database&,
    transaction&,
    const dependencies&,
    const package_name&,
    const function<find_prereq_database_function>& = {});
}

#endif // BPKG_PKG_CONFIGURE_HXX
