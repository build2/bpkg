// file      : bpkg/pkg-verify.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_VERIFY_HXX
#define BPKG_PKG_VERIFY_HXX

#include <libbpkg/manifest.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/pkg-verify-options.hxx>

namespace bpkg
{
  int
  pkg_verify (const pkg_verify_options&, cli::scanner& args);

  // Verify archive is a valid package and return its manifest. If requested,
  // expand the file-referencing manifest values (description, changes, etc),
  // setting them to the contents of files they refer to. Throw failed if
  // invalid or if something goes wrong. If diag is false, then don't issue
  // diagnostics about the reason why the package is invalid.
  //
  package_manifest
  pkg_verify (const common_options&,
              const path& archive,
              bool expand_values,
              bool ignore_unknown,
              bool diag = true);

  // Similar to the above but verifies that a source directory is a valid
  // package. Note that it doesn't enforce the <name>-<version> form for the
  // directory itself.
  //
  package_manifest
  pkg_verify (const dir_path& source, bool ignore_unknown, bool diag = true);
}

#endif // BPKG_PKG_VERIFY_HXX
