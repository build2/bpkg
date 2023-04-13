// file      : bpkg/pkg-configure.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_CONFIGURE_HXX
#define BPKG_PKG_CONFIGURE_HXX

#include <libbpkg/manifest.hxx>     // version
#include <libbpkg/package-name.hxx>

#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx> // variable_overrides

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

  // Given dependencies of a package, return its prerequisite packages,
  // configuration variables that resulted from selection of these
  // prerequisites (import, reflection, etc), and sources of the configuration
  // variables resulted from evaluating the reflect clauses. See
  // pkg_configure() for the semantics of the dependency list. Fail if for
  // some of the dependency alternative lists there is no satisfactory
  // alternative (all its dependencies are configured, satisfy the respective
  // constraints, etc).
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

  // Return the "would be" state for packages that would be configured
  // by this stage.
  //
  using find_package_state_function =
    optional<pair<package_state, package_substate>> (
      const shared_ptr<selected_package>&);

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
                               const function<find_database_function>&,
                               const function<find_package_state_function>&);


  // Configure the package, update its state, and commit the transaction.
  //
  // This is a lower-level version meant for sharing the same build context
  // to configure multiple packages (in the dependency order).
  //
  // Note: variable_overrides must include config.config.disfigure, if
  //       required.
  //
  // Note: expects all the non-external packages to be configured to be
  //       already unpackged (for subproject discovery).
  //
  void
  pkg_configure (const common_options&,
                 database&,
                 transaction&,
                 const shared_ptr<selected_package>&,
                 configure_prerequisites_result&&,
                 const unique_ptr<build2::context>&,
                 const build2::variable_overrides&,
                 bool simulate);

  // Create a build context suitable for configuring packages.
  //
  unique_ptr<build2::context>
  pkg_configure_context (
    const common_options&,
    strings&& cmd_vars,
    const function<build2::context::var_override_function>& = nullptr);

  // This is a higher-level version meant for configuring a single package.
  //
  // Note: loads selected packages.
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
}

#endif // BPKG_PKG_CONFIGURE_HXX
