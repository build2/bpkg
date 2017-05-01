// file      : bpkg/fetch.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_FETCH_HXX
#define BPKG_FETCH_HXX

#include <libbpkg/manifest.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/common-options.hxx>

namespace bpkg
{
  repository_manifests
  fetch_repositories (const dir_path&, bool ignore_unknown);

  pair<repository_manifests, string/*checksum*/>
  fetch_repositories (const common_options&,
                      const repository_location&,
                      bool ignore_unknown);

  package_manifests
  fetch_packages (const dir_path&, bool ignore_unknown);

  pair<package_manifests, string/*checksum*/>
  fetch_packages (const common_options&,
                  const repository_location&,
                  bool ignore_unknown);

  signature_manifest
  fetch_signature (const common_options&,
                   const repository_location&,
                   bool ignore_unknown);

  path
  fetch_archive (const common_options&,
                 const repository_location&,
                 const path& archive,
                 const dir_path& destdir);
}

#endif // BPKG_FETCH_HXX
