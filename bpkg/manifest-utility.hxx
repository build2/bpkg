// file      : bpkg/manifest-utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_MANIFEST_UTILITY_HXX
#define BPKG_MANIFEST_UTILITY_HXX

#include <libbpkg/manifest.hxx>
#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

namespace bpkg
{
  extern const path repositories_file; // repositories.manifest
  extern const path packages_file;     // packages.manifest
  extern const path signature_file;    // signature.manifest
  extern const path manifest_file;     // manifest

  // Package naming schemes.
  //
  enum class package_scheme
  {
    none,
    sys
  };

  // Extract scheme from [<scheme>:]<package>. Position the pointer right after
  // the scheme end if present, otherwise leave unchanged.
  //
  package_scheme
  parse_package_scheme (const char*&);

  // Extract the package name and version components from <name>[/<version>].
  // Diagnose invalid components and throw failed.
  //
  package_name
  parse_package_name (const char*, bool allow_version = true);

  inline package_name
  parse_package_name (const string& s, bool allow_version = true)
  {
    return parse_package_name (s.c_str (), allow_version);
  }

  // Return empty version if none is specified.
  //
  version
  parse_package_version (const char*,
                         bool allow_wildcard = false,
                         bool fold_zero_revision = true);

  inline version
  parse_package_version (const string& s,
                         bool allow_wildcard = false,
                         bool fold_zero_revision = true)
  {
    return parse_package_version (s.c_str (),
                                  allow_wildcard,
                                  fold_zero_revision);
  }

  // Extract the package constraint from either <name>[/<version>] or
  // <name><version-constraint> forms, unless version_only is true. For the
  // former case return the `== <version>` constraint. Return nullopt if only
  // the package name is specified.
  //
  optional<version_constraint>
  parse_package_version_constraint (const char*,
                                    bool allow_wildcard = false,
                                    bool fold_zero_revision = true,
                                    bool version_only = false);

  // If the passed location is a relative local path, then assume this is a
  // relative path to the repository directory and complete it based on the
  // current working directory. Diagnose invalid locations and throw failed.
  //
  repository_location
  parse_location (const string&, optional<repository_type>);

  // Return the repository state subdirectory for the specified location as it
  // appears under .bpkg/repos/ in the bpkg configuration. Return empty
  // directory if the repository type doesn't have any state.
  //
  // Note that the semantics used to produce this name is repository type-
  // specific and can base on the repository canonical name or (potentially a
  // subset of) the location URL. In particular, a state directory could be
  // shared by multiple repository locations of the same type.
  //
  dir_path
  repository_state (const repository_location&);

  // Return true if the argument is a valid repository canonical name.
  //
  bool
  repository_name (const string&);

  // Return the version of a package as provided by the build2 version module.
  // Return nullopt if the version module is disabled for the package (or the
  // build2 project directory doesn't contain the manifest file). Fail if the
  // directory is not a build2 project.
  //
  // Note that if the package directory is under the version control, then the
  // resulting version may be populated with the snapshot information (see
  // libbutl/standard-version.mxx for more details). Thus, this function can
  // be used for fixing up the package manifest version.
  //
  class common_options;

  optional<version>
  package_version (const common_options&, const dir_path&);
}

#endif // BPKG_MANIFEST_UTILITY_HXX
