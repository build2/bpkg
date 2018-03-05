// file      : bpkg/manifest-utility.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_MANIFEST_UTILITY_HXX
#define BPKG_MANIFEST_UTILITY_HXX

#include <libbpkg/manifest.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

namespace bpkg
{
  extern const path repositories_file; // repositories.manifest
  extern const path packages_file;     // packages.manifest
  extern const path signature_file;    // signature.manifest

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

  // Extract name and version components from <name>[/<version>].
  //
  string
  parse_package_name (const char*);

  inline string
  parse_package_name (const string& s)
  {
    return parse_package_name (s.c_str ());
  }

  version
  parse_package_version (const char*);

  inline version
  parse_package_version (const string& s)
  {
    return parse_package_version (s.c_str ());
  }

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
  // shared by multiple repository locations of the same type (@@ TODO: if we
  // ever do this, then we will need to complicate the removal logic).
  //
  dir_path
  repository_state (const repository_location&);

  // Return true if the argument is a valid repository canonical name.
  //
  bool
  repository_name (const string&);
}

#endif // BPKG_MANIFEST_UTILITY_HXX
