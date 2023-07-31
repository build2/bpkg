// file      : bpkg/package-query.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PACKAGE_QUERY_HXX
#define BPKG_PACKAGE_QUERY_HXX

#include <odb/core.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>
#include <bpkg/database.hxx>

#include <bpkg/common-options.hxx>

namespace bpkg
{
  // Query the available packages that optionally satisfy the specified
  // version constraint and return them in the version descending order, by
  // default. Note that a stub satisfies any constraint.
  //
  // By default if the revision is not explicitly specified for the version
  // constraint, then compare ignoring the revision. The idea is that when the
  // user runs 'bpkg build libfoo/1' and there is 1+1 available, it should
  // just work. The user shouldn't have to spell the revision
  // explicitly. Similarly, when we have 'depends: libfoo == 1', then it would
  // be strange if 1+1 did not satisfy this constraint. The same for libfoo <=
  // 1 -- 1+1 should satisfy.
  //
  // Note that by default we compare ignoring the iteration, as it can not be
  // specified in the manifest/command line. This way the latest iteration
  // will always be picked up.
  //
  // Pass true as the revision argument to query the exact available package
  // version, also comparing the version revision and iteration.
  //
  odb::result<available_package>
  query_available (database&,
                   const package_name&,
                   const optional<version_constraint>&,
                   bool order = true,
                   bool revision = false);

  // Only return packages that are in the specified repository fragments, their
  // complements or prerequisites (if prereq is true), recursively. While you
  // could maybe come up with a (barely comprehensible) view/query to achieve
  // this, doing it on the "client side" is definitely more straightforward.
  //
  vector<shared_ptr<available_package>>
  filter (const shared_ptr<repository_fragment>&,
          odb::result<available_package>&&,
          bool prereq = true);

  pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>
  filter_one (const shared_ptr<repository_fragment>&,
              odb::result<available_package>&&,
              bool prereq = true);

  shared_ptr<repository_fragment>
  filter (const shared_ptr<repository_fragment>&,
          const shared_ptr<available_package>&,
          bool prereq = true);

  vector<pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>>
  filter (const vector<shared_ptr<repository_fragment>>&,
          odb::result<available_package>&&,
          bool prereq = true);

  pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>
  filter_one (const vector<shared_ptr<repository_fragment>>&,
              odb::result<available_package>&&,
              bool prereq = true);

  // Try to find packages that optionally satisfy the specified version
  // constraint in multiple databases, suppressing duplicates. Return the list
  // of packages and repository fragments in which each was found in the
  // package version descending order or empty list if none were found. Note
  // that a stub satisfies any constraint.
  //
  // Note that we return (loaded) lazy_shared_ptr in order to also convey
  // the database to which it belongs.
  //
  available_packages
  find_available (const linked_databases&,
                  const package_name&,
                  const optional<version_constraint>&);

  // As above but only look for packages from the specified list of repository
  // fragments, their prerequisite repositories, and their complements,
  // recursively (note: recursivity applies to complements, not prerequisites).
  //
  using config_repo_fragments =
    database_map<vector<shared_ptr<repository_fragment>>>;

  available_packages
  find_available (const package_name&,
                  const optional<version_constraint>&,
                  const config_repo_fragments&,
                  bool prereq = true);

  // As above but only look for packages from a single repository fragment,
  // its prerequisite repositories, and its complements, recursively (note:
  // recursivity applies to complements, not prerequisites). Doesn't provide
  // the repository fragments the packages come from.
  //
  // It is assumed that the repository fragment lazy pointer contains the
  // database information.
  //
  vector<shared_ptr<available_package>>
  find_available (const package_name&,
                  const optional<version_constraint>&,
                  const lazy_shared_ptr<repository_fragment>&,
                  bool prereq = true);

  // As above but only look for a single package from the specified repository
  // fragment, its prerequisite repositories, and their complements,
  // recursively (note: recursivity applies to complements, not
  // prerequisites). Return the package and the repository fragment in which
  // it was found or NULL for both if not found.
  //
  // It is assumed that the repository fragment lazy pointer contains the
  // database information.
  //
  pair<shared_ptr<available_package>,
       lazy_shared_ptr<repository_fragment>>
  find_available_one (const package_name&,
                      const optional<version_constraint>&,
                      const lazy_shared_ptr<repository_fragment>&,
                      bool prereq = true,
                      bool revision = false);

  // As above but look for a single package from a list of repository
  // fragments.
  //
  pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>
  find_available_one (database&,
                      const package_name&,
                      const optional<version_constraint>&,
                      const vector<shared_ptr<repository_fragment>>&,
                      bool prereq = true,
                      bool revision = false);

  // As above but look for a single package in multiple databases from their
  // respective root repository fragments.
  //
  pair<shared_ptr<available_package>,
       lazy_shared_ptr<repository_fragment>>
  find_available_one (const linked_databases&,
                      const package_name&,
                      const optional<version_constraint>&,
                      bool prereq = true,
                      bool revision = false);

  // Try to find an available package corresponding to the specified selected
  // package and, if not found, return a transient one. The search is
  // performed in the ultimate dependent configurations of the selected
  // package (see dependent_repo_configs() for details).
  //
  // NOTE: repo_configs needs to be filled prior to the function call.
  //
  shared_ptr<available_package>
  find_available (const common_options&,
                  database&,
                  const shared_ptr<selected_package>&);

  // As above but also pair the available package with the repository fragment
  // the available package comes from. Note that the package locations list is
  // left empty and that the returned repository fragment could be NULL if the
  // package is an orphan.
  //
  // NOTE: repo_configs needs to be filled prior to the function call.
  //
  pair<shared_ptr<available_package>,
       lazy_shared_ptr<repository_fragment>>
  find_available_fragment (const common_options&,
                           database&,
                           const shared_ptr<selected_package>&);

  // Try to find packages in multiple databases, traversing the explicitly and
  // implicitly linked databases recursively and suppressing duplicates and,
  // optionally, older package revisions. Return the list of packages and
  // repository fragments in which each was found in the package version
  // descending order or empty list if none were found.
  //
  // Note that we return (loaded) lazy_shared_ptr in order to also convey
  // the database to which it belongs.
  //
  available_packages
  find_available_all (const linked_databases&,
                      const package_name&,
                      bool suppress_older_revisions = true);

  // Create a transient (or fake, if you prefer) available_package object
  // corresponding to the specified selected object. Note that the package
  // locations list is left empty and that the returned repository fragment
  // could be NULL if the package is an orphan.
  //
  // Note that the repository fragment is searched in the ultimate dependent
  // configurations of the selected package (see dependent_repo_configs() for
  // details).
  //
  // Also note that in our model we assume that make_available_fragment() is
  // only called if there is no real available_package. This makes sure that
  // if the package moves (e.g., from testing to stable), then we will be
  // using stable to resolve its dependencies.
  //
  // NOTE: repo_configs needs to be filled prior to the function call.
  //
  pair<shared_ptr<available_package>,
       lazy_shared_ptr<repository_fragment>>
  make_available_fragment (const common_options&,
                           database&,
                           const shared_ptr<selected_package>&);

  // Try to find an available stub package in the imaginary system repository.
  // Such a repository contains stubs corresponding to the system packages
  // specified by the user on the command line with version information
  // (sys:libfoo/1.0, ?sys:libfoo/* but not ?sys:libfoo; the idea is that a
  // real stub won't add any extra information to such a specification so we
  // shouldn't insist on its presence). Semantically this imaginary repository
  // complements all real repositories.
  //
  extern vector<shared_ptr<available_package>> imaginary_stubs;

  shared_ptr<available_package>
  find_imaginary_stub (const package_name&);

  // Try to find an available package in the existing packages registry. Such
  // a registry is configuration-specific and contains package versions
  // specified by the user on the command line as archives or directories for
  // specific configurations (see pkg-build for details on such packages).
  //
  // Note that semantically such a registry can be considered as an imaginary
  // repository which complements all the real repositories fetched in the
  // respective configuration. Also note that normally this repository is used
  // first (by calling this function) when trying to resolve a dependency
  // package, prior to searching in the real repositories.
  //
  extern vector<pair<reference_wrapper<database>,
                     shared_ptr<available_package>>> existing_packages;

  pair<shared_ptr<available_package>, lazy_shared_ptr<repository_fragment>>
  find_existing (database&,
                 const package_name&,
                 const optional<version_constraint>&);

  inline pair<shared_ptr<available_package>,
              lazy_shared_ptr<repository_fragment>>
  find_existing (const package_name& n,
                 const optional<version_constraint>& c,
                 const lazy_shared_ptr<repository_fragment>& rf)
  {
    return find_existing (rf.database (), n, c);
  }

  // Configurations to use as the repository information sources.
  //
  // The list normally contains the current configurations and configurations
  // of the specified on the command line build-to-hold packages (ultimate
  // dependents).
  //
  // For ultimate dependents we use configurations in which they are being
  // built as a source of the repository information. For dependency packages
  // we use configurations of their ultimate dependents.
  //
  extern linked_databases repo_configs;

  // Return the ultimate dependent configurations for packages in this
  // configuration.
  //
  // Specifically, this is an intersection of all the dependent configurations
  // for the specified configuration (see database::dependent_configs() for
  // details) and configurations which contain repository information
  // (repo_configs).
  //
  linked_databases
  dependent_repo_configs (database&);
}

#endif // BPKG_PACKAGE_QUERY_HXX
