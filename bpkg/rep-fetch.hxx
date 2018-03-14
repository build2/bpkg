// file      : bpkg/rep-fetch.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_REP_FETCH_HXX
#define BPKG_REP_FETCH_HXX

#include <libbpkg/manifest.hxx>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // database
#include <bpkg/utility.hxx>

#include <bpkg/rep-fetch-options.hxx>

namespace bpkg
{
  int
  rep_fetch (const rep_fetch_options&, cli::scanner& args);

  // Fetch and authenticate repositories and packages manifests.
  //
  // If conf is NULL, then assume not running in a bpkg configuration. If it
  // is empty, then check if the bpkg configuration exists in the current
  // working directory.
  //
  class certificate;

  struct rep_fetch_data
  {
    using repository = repository_manifest;

    struct package
    {
      package_manifest manifest;
      string repository_fragment; // See package_location::fragment.
    };

    std::vector<repository>       repositories;
    std::vector<package>          packages;

    // For base repo (can be NULL).
    //
    shared_ptr<const bpkg::certificate> certificate;
  };

  rep_fetch_data
  rep_fetch (const common_options&,
             const dir_path* conf,
             const repository_location&,
             bool ignore_unknown);

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
             const dir_path& conf,
             database&,
             const vector<repository_location>&,
             bool shallow,
             const optional<string>& reason);
}

#endif // BPKG_REP_FETCH_HXX
