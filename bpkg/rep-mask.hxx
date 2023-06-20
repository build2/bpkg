// file      : bpkg/rep-mask.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_REP_MASK_HXX
#define BPKG_REP_MASK_HXX

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // database, repository
#include <bpkg/utility.hxx>

namespace bpkg
{
  // Note: not a command (at least not yet).
  //
  // Mask repositories to pretend they don't exist in the configurations that
  // are used as the repository information sources (repo_configs;
  // repositories argument) and/or specific configurations
  // (config_uuid_repositories argument). Also mask their complement and
  // prerequisite repositories, recursively, excluding those which are
  // complements and/or prerequisites of other unmasked repositories. The
  // repositories can be specified either as repository location canonical
  // names or URLs. Issue diagnostics and fail if any of the specified
  // repositories don't exist in any configuration.
  //
  // Notes:
  //
  // - The current configurations are only used to resolve the configuration
  //   UUIDs, if any.
  //
  // - A repository may end up being masked in one configuration but not in
  //   another.
  //
  // - Using a canonical name potentially masks repositories with different
  //   URLs in different configurations (think of local and remote pkg
  //   repository locations).
  //
  // - Using a URL potentially masks repositories with different canonical
  //   names in the same configuration (think of directory and local git
  //   repository locations).
  //
  // NOTE: repo_configs needs to be filled prior to the function call.
  //
  void
  rep_mask (const strings& repositories,             // <rep>
            const strings& config_uuid_repositories, // <config-uuid>=<rep>
            linked_databases& current_configs);

  // Return true if a repository is masked in the specified configuration.
  //
  bool
  rep_masked (database&, const shared_ptr<repository>&);

  // Note: the argument must refer to a persistent object which incorporates
  // the configuration information (database).
  //
  bool
  rep_masked (const lazy_weak_ptr<repository>&);

  // Return true if a repository fragment in the specified configuration
  // belongs to the masked repositories only and is therefore masked (see
  // package.hxx for the fragment/repository relationship details).
  //
  bool
  rep_masked_fragment (database&, const shared_ptr<repository_fragment>&);

  // Note: the argument must refer to a persistent object which incorporates
  // the configuration information (database).
  //
  bool
  rep_masked_fragment (const lazy_shared_ptr<repository_fragment>&);
}

#endif // BPKG_REP_MASK_HXX
