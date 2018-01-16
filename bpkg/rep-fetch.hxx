// file      : bpkg/rep-fetch.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
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
  // If conf is NULL, then assume not running in a bpkg configuration. If it
  // is empty, then check if the bpkg configuration exists in the current
  // working directory.
  //
  class certificate;

  struct rep_fetch_data
  {
    std::vector<repository_manifest>    repositories;
    std::vector<package_manifest>       packages;
    shared_ptr<const bpkg::certificate> certificate; // Can be NULL.
  };

  rep_fetch_data
  rep_fetch (const common_options& co,
             const dir_path* conf,
             const repository_location& rl,
             bool ignore_unknown);
}

#endif // BPKG_REP_FETCH_HXX
