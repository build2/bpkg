// file      : bpkg/manifest-utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_MANIFEST_UTILITY_HXX
#define BPKG_MANIFEST_UTILITY_HXX

#include <libbpkg/manifest.hxx>
#include <libbpkg/package-name.hxx>

#include <libbutl/b.hxx> // b_info_flags

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

namespace bpkg
{
  extern const path repositories_file; // repositories.manifest
  extern const path packages_file;     // packages.manifest
  extern const path signature_file;    // signature.manifest
  extern const path manifest_file;     // manifest

  using butl::b_info_flags;

  // Obtain build2 projects info for package source or output directories.
  //
  vector<package_info>
  package_b_info (const common_options&, const dir_paths&, b_info_flags);

  // As above but return the info for a single package directory.
  //
  inline package_info
  package_b_info (const common_options& o, const dir_path& d, b_info_flags fl)
  {
    vector<package_info> r (package_b_info (o, dir_paths ({d}), fl));
    return move (r[0]);
  }

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
                         version::flags fl = version::fold_zero_revision);

  inline version
  parse_package_version (const string& s,
                         bool allow_wildcard = false,
                         version::flags fl = version::fold_zero_revision)
  {
    return parse_package_version (s.c_str (), allow_wildcard, fl);
  }

  // Extract the package constraint from either <name>[/<version>] or
  // <name><version-constraint> forms, unless version_only is true. For the
  // former case return the `== <version>` constraint. Return nullopt if only
  // the package name is specified.
  //
  optional<version_constraint>
  parse_package_version_constraint (
    const char*,
    bool allow_wildcard = false,
    version::flags = version::fold_zero_revision,
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

  // Return the versions of packages as provided by the build2 version module
  // together with the build2 project info the versions originate from (in
  // case the caller may want to reuse it). Return nullopt as a package
  // version if the version module is disabled for the package (or the build2
  // project directory doesn't contain the manifest file). Fail if any of the
  // specified directories is not a build2 project.
  //
  // Note that if a package directory is under the version control, then the
  // resulting version may be populated with the snapshot information (see
  // libbutl/standard-version.hxx for more details). Thus, this function can
  // be used for fixing up the package manifest versions.
  //
  class common_options;

  struct package_version_info
  {
    optional<bpkg::version> version;
    package_info            info;
  };

  using package_version_infos = vector<package_version_info>;

  package_version_infos
  package_versions (const common_options&, const dir_paths&, b_info_flags);

  // As above but return the version of a single package.
  //
  inline package_version_info
  package_version (const common_options& o, const dir_path& d, b_info_flags fl)
  {
    package_version_infos r (package_versions (o, dir_paths ({d}), fl));
    return move (r[0]);
  }

  // Caclulate the checksum of the manifest file located in the package source
  // directory and the subproject set (see package::manifest_checksum).
  //
  // Pass the build2 project info for the package, if available, to speed up
  // the call and NULL otherwise (in which case it will be queried by the
  // implementation). In the former case it is assumed that the package info
  // has been retrieved with the b_info_flags::subprojects flag.
  //
  string
  package_checksum (const common_options&,
                    const dir_path& src_dir,
                    const package_info*);

  // Calculate the checksum of the buildfiles using the *-build manifest
  // values and, if the package source directory is specified (not empty),
  // build-file values. If the package source directory is specified, then
  // also use the files it contains for unspecified values. If additionally
  // the alt_naming flag is specified, then verify the package's buildfile
  // naming scheme against its value and fail on mismatch.
  //
  string
  package_buildfiles_checksum (const optional<string>& bootstrap_build,
                               const optional<string>& root_build,
                               const vector<buildfile>& buildfiles,
                               const dir_path& src_dir = {},
                               const vector<path>& buildfile_paths = {},
                               optional<bool> alt_naming = nullopt);

  // Load the package's buildfiles for unspecified manifest values. Throw
  // std::runtime_error for underlying errors (unable to find bootstrap.build,
  // unable to read from file, etc). Optionally convert paths used in the
  // potential error description to be relative to the package source
  // directory.
  //
  // Note that before calling this function you need to expand the build-file
  // manifest values into the respective *-build values, for example, by
  // calling manifest::load_files().
  //
  void
  load_package_buildfiles (package_manifest&,
                           const dir_path& src_dir,
                           bool err_path_relative = false);
}

#endif // BPKG_MANIFEST_UTILITY_HXX
