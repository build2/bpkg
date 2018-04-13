// file      : bpkg/fetch.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_FETCH_HXX
#define BPKG_FETCH_HXX

#include <libbutl/process.mxx>

#include <libbpkg/manifest.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/common-options.hxx>

namespace bpkg
{
  // Repository type pkg (fetch-pkg.cxx).
  //

  pkg_repository_manifests
  pkg_fetch_repositories (const dir_path&, bool ignore_unknown);

  pair<pkg_repository_manifests, string /* checksum */>
  pkg_fetch_repositories (const common_options&,
                          const repository_location&,
                          bool ignore_unknown);

  pkg_package_manifests
  pkg_fetch_packages (const dir_path&, bool ignore_unknown);

  pair<pkg_package_manifests, string /* checksum */>
  pkg_fetch_packages (const common_options&,
                      const repository_location&,
                      bool ignore_unknown);

  signature_manifest
  pkg_fetch_signature (const common_options&,
                       const repository_location&,
                       bool ignore_unknown);

  void
  pkg_fetch_archive (const common_options&,
                     const repository_location&,
                     const path& archive,
                     const path& dest);

  // Repository type git (fetch-git.cxx).
  //

  // Create a git repository in the specified directory and prepare it for
  // fetching from the specified repository location. Note that the repository
  // URL fragment is neither used nor validated.
  //
  void
  git_init (const common_options&,
            const repository_location&,
            const dir_path&);

  // Fetch a git repository in the specifid directory (previously created by
  // git_init()) for the references obtained from the repository URL fragment
  // returning commit ids these references resolve to. After fetching the
  // repository working tree state is unspecified (see git_checkout ()).
  //
  // Note that submodules are not fetched.
  //
  strings
  git_fetch (const common_options&,
             const repository_location&,
             const dir_path&);

  // Checkout the specified commit previously fetched by git_fetch().
  //
  // Note that submodules are not checked out.
  //
  void
  git_checkout (const common_options&,
                const dir_path&,
                const string& commit);

  // Fetch (if necessary) and checkout submodules, recursively, in a working
  // tree previously checked out by git_checkout().
  //
  void
  git_checkout_submodules (const common_options&, const dir_path&);

  // Low-level fetch API (fetch.cxx).
  //

  // Start the process of fetching the specified URL. If out is empty, then
  // fetch to STDOUT. In this case also don't show any progress unless we are
  // running verbose.
  //
  butl::process
  start_fetch (const common_options& o,
               const string& url,
               const path& out = path ());
}

#endif // BPKG_FETCH_HXX
