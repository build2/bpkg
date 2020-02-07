// file      : bpkg/rep-remove.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_REP_REMOVE_HXX
#define BPKG_REP_REMOVE_HXX

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // database, transaction, repository
#include <bpkg/utility.hxx>

#include <bpkg/rep-remove-options.hxx>

namespace bpkg
{
  int
  rep_remove (const rep_remove_options&, cli::scanner& args);

  // Remove a repository if it is not reachable from the root (and thus is not
  // required by any user-added repository), also removing its unused
  // repository fragments.
  //
  void
  rep_remove (const dir_path& conf,
              transaction&,
              const shared_ptr<repository>&);

  // Remove a repository fragment if it is not referenced by any repository,
  // also removing its unreachable complements and prerequisites.
  //
  void
  rep_remove_fragment (const dir_path& conf,
                       transaction&,
                       const shared_ptr<repository_fragment>&);

  // Bring the configuration to the clean state as if repositories were added
  // but never fetched. Leave selected packages intact.
  //
  // Specifically:
  //
  // - Clean prerequisite and complement repository sets for the top-level
  //   repositories.
  //
  // - Remove all repositories except the top-level ones and the root.
  //
  // - Remove all repository fragments except the root.
  //
  // - Remove all repository state directories (regardless of whether they
  //   actually relate to any existing repositories).
  //
  // - Remove all available packages.
  //
  void
  rep_remove_clean (const common_options&,
                    const dir_path& conf,
                    database&,
                    bool quiet = true);

  // Remove a repository fragment from locations of the available packages it
  // contains. Remove packages that come from only this repository fragment.
  //
  void
  rep_remove_package_locations (transaction&, const string& fragment_name);
}

#endif // BPKG_REP_REMOVE_HXX
