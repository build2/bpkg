// file      : bpkg/pkg-verify.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_VERIFY_HXX
#define BPKG_PKG_VERIFY_HXX

#include <libbutl/manifest-forward.hxx>

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
  // libbpkg/manifest.hxx), load the bootstrap, root, and config/*.build
  // buildfiles into the respective *-build values, and complete the
  // dependency constraints.
  //
  // Throw not_package (derived from failed) if this doesn't look like a
  // package. Throw plain failed if this does looks like a package but
  // something about it is invalid or if something else goes wrong.
  //
  // Issue diagnostics according the diag_level as follows:
  //
  // 0 - Suppress all errors messages except for underlying system errors.
  // 1 - Suppress error messages about the reason why this is not a package.
  // 2 - Suppress no error messages.
  //
  class not_package: public failed {};

  package_manifest
  pkg_verify (const common_options&,
              const path& archive,
              bool ignore_unknown,
              bool expand_values,
              bool load_buildfiles,
              bool complete_depends = true,
              int diag_level = 2);

  // Similar to the above but verifies that a source directory is a valid
  // package. Always translates the package version and completes dependency
  // constraints but doesn't expand the file-referencing manifest values. Note
  // that it doesn't enforce the <name>-<version> form for the directory
  // itself.
  //
  package_manifest
  pkg_verify (const common_options&,
              const dir_path& source,
              bool ignore_unknown,
              bool load_buildfiles,
              const function<package_manifest::translate_function>&,
              int diag_level = 2);

  // Pre-parse the package manifest and return the name value pairs list,
  // stripping the format version and the end-of-manifest/stream pairs. Also
  // verify that the package is compatible with the current toolchain and
  // issue diagnostics and throw failed if it is not. Pass through the
  // manifest_parsing and io_error exceptions, so that the caller can decide
  // how to handle them (for example, ignore them if the manifest-printing
  // process has failed, etc).
  //
  // To omit the package location from the diagnostics, pass an empty path as
  // the what argument.
  //
  vector<butl::manifest_name_value>
  pkg_verify (const common_options&,
              butl::manifest_parser&,
              const path& what,
              int diag_level = 2);
}

#endif // BPKG_PKG_VERIFY_HXX
