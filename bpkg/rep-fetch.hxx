// file      : bpkg/rep-fetch.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_REP_FETCH_HXX
#define BPKG_REP_FETCH_HXX

#include <libbpkg/manifest.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/rep-fetch-options.hxx>

namespace bpkg
{
  int
  rep_fetch (const rep_fetch_options&, cli::scanner& args);

  // Fetch and authenticate repositories and packages manifests.
  //
  // If configuration directory is NULL, then assume not running in a bpkg
  // configuration.
  //
  class certificate;

  struct rep_fetch_data
  {
    struct fragment
    {
      // Empty for fragment-less repositories.
      //
      string id;
      string friendly_name; // User-friendly fragment name (e.g, tag, etc).

      vector<repository_manifest> repositories;
      vector<package_manifest>    packages;

      // Empty if the build2 project info is not available for the packages.
      // Currently we only retrieve it for the directory and version control
      // based repositories, but only if the current build2 version is
      // satisfactory for all the repository packages.
      //
      vector<package_info>        package_infos;
    };

    vector<fragment> fragments;

    // For pkg repositories (can be nullopt/NULL).
    //
    optional<string> certificate_pem;
    shared_ptr<const bpkg::certificate> certificate; // Authenticated.
  };

  // If requested, verify that all manifest entries are recognized and the
  // packages are compatible with the current toolchain. Also, if requested,
  // expand the file-referencing package manifest values (description,
  // changes, etc), setting them to the contents of files they refer to and
  // set the potentially absent description-type value to the effective
  // description type (see libbpkg/manifest.hxx) and load the bootstrap, root,
  // and config/*.build buildfiles into the respective *-build values. Note
  // that for pkg repositories such values are expanded/loaded at the
  // repository creation time.
  //
  rep_fetch_data
  rep_fetch (const common_options&,
             const dir_path* configuration,
             const repository_location&,
             bool ignore_unknown,
             bool ignore_toolchain,
             bool expand_values,
             bool load_buildfiles);

  // Add (or update) repository locations to the configuration and fetch
  // them. If shallow is true, then don't fetch their prerequisite and/or
  // complements unless the respective sets have changed. On failure clean up
  // the configuration (see rep_remove_clean() for details). Note that it
  // should be called in session.
  //
  // If reason is absent, then don't print the "fetching ..." progress line.
  //
  void
  rep_fetch (const common_options&,
             database&,
             const vector<repository_location>&,
             bool shallow,
             const optional<string>& reason);
}

#endif // BPKG_REP_FETCH_HXX
