// file      : bpkg/pkg-verify.hxx -*- C++ -*-
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
  // setting them to the contents of files they refer to, set the potentially
  // absent description-type value to the effective description type (see
  // libbpkg/manifest.hxx), and complete the dependency constraints. Throw
  // failed if invalid or if something goes wrong. If diag is false, then
  // don't issue diagnostics about the reason why the package is invalid.
  //
  package_manifest
  pkg_verify (const common_options&,
              const path& archive,
              bool ignore_unknown,
              bool expand_values,
              bool complete_depends = true,
              bool diag = true);

  // Similar to the above but verifies that a source directory is a valid
  // package. Always translates the package version and completes dependency
  // constraints but doesn't expand the file-referencing manifest values. Note
  // that it doesn't enforce the <name>-<version> form for the directory
  // itself.
  //
  package_manifest
  pkg_verify (const dir_path& source,
              bool ignore_unknown,
              const function<package_manifest::translate_function>&,
              bool diag = true);
}

#endif // BPKG_PKG_VERIFY_HXX
