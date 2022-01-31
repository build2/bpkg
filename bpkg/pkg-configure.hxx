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

  // The custom search function. If specified, it is called by pkg_configure()
  // to obtain the database to search for the prerequisite in, instead of
  // searching for it in the linked databases, recursively. If the function
  // returns NULL, then fallback to the recursive search through the linked
  // databases.
  //
  using find_database_function = database* (database&,
                                            const package_name&,
                                            bool buildtime);

  // Configure the package, update its state, and commit the transaction.
  //
  // The package dependency constraints are expected to be complete. Empty
  // dependency alternatives lists are allowed and are ignored (see pkg-build
  // for the use-case).
  //
  void
  pkg_configure (const common_options&,
                 database&,
                 transaction&,
                 const shared_ptr<selected_package>&,
                 const dependencies&,
                 const string& bootstrap_build,
                 const optional<string>& root_build,
                 const strings& config_vars,
                 bool simulate,
                 const function<find_database_function>& = {});

  // Configure a system package and commit the transaction.
  //
  shared_ptr<selected_package>
  pkg_configure_system (const package_name&,
                        const version&,
                        database&,
                        transaction&);
}

#endif // BPKG_PKG_CONFIGURE_HXX
