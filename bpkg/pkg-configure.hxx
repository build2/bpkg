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
#include <bpkg/package-skeleton.hxx>
#include <bpkg/pkg-configure-options.hxx>

namespace bpkg
{
  int
  pkg_configure (const pkg_configure_options&, cli::scanner& args);

  // Configure a system package and commit the transaction.
  //
  shared_ptr<selected_package>
  pkg_configure_system (const package_name&,
                        const version&,
                        database&,
                        transaction&);

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
  // The package dependency constraints are expected to be complete.
  //
  // The dependencies argument may contain pre-selected dependency
  // alternatives (with the potential empty entries for the toolchain
  // build-time dependencies or for dependencies with all the alternatives
  // disabled; see pkg-build for the use-case). In this case the number of
  // dependency alternatives for each dependency must be 1 (or 0) and the
  // alternatives argument must be specified. The alternatives argument must
  // be parallel to the dependencies argument and specify indexes of the
  // selected alternatives.
  //
  // If prerequisites corresponding to the previous configured state of the
  // package are specified, then for each depends value try to select an
  // alternative where dependencies all belong to this list (the "recreate
  // dependency decisions" mode). Failed that, select an alternative as if no
  // prerequisites are specified (the "make dependency decisions" mode).
  //
  void
  pkg_configure (const common_options&,
                 database&,
                 transaction&,
                 const shared_ptr<selected_package>&,
                 const dependencies&,
                 const vector<size_t>* alternatives,
                 package_skeleton&&,
                 const vector<package_name>* prev_prerequisites,
                 bool disfigured,
                 bool simulate,
                 const function<find_database_function>& = {});


  // Given dependencies of a package, return its prerequisite packages,
  // configuration variables that resulted from selection of these
  // prerequisites (import, reflection, etc), and sources of the configuration
  // variables resulted from evaluating the reflect clauses. See
  // pkg_configure() for the semantics of the dependency list. Fail if for
  // some of the dependency alternative lists there is no satisfactory
  // alternative (all its dependencies are configured, satisfy the respective
  // constraints, etc).
  //
  struct configure_prerequisites_result
  {
    package_prerequisites   prerequisites;
    strings                 config_variables; // Note: name and value.

    // Only contains sources of configuration variables collected using the
    // package skeleton, excluding those user-specified variables which are
    // not the project variables for the specified package (module
    // configuration variables, etc). Thus, it is not parallel to the
    // config_variables member.
    //
    vector<config_variable> config_sources; // Note: name and source.
  };

  // Note: loads selected packages.
  //
  configure_prerequisites_result
  pkg_configure_prerequisites (const common_options&,
                               database&,
                               transaction&,
                               const dependencies&,
                               const vector<size_t>* alternatives,
                               package_skeleton&&,
                               const vector<package_name>* prev_prerequisites,
                               bool simulate,
                               const function<find_database_function>&);

  void
  pkg_configure (const common_options&,
                 database&,
                 transaction&,
                 const shared_ptr<selected_package>&,
                 configure_prerequisites_result&&,
                 bool disfigured,
                 bool simulate);
}

#endif // BPKG_PKG_CONFIGURE_HXX
