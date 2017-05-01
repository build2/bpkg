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

  version
  parse_package_version (const char*);

  // First use the passed location as is. If the result is relative,
  // then assume this is a relative path to the repository directory
  // and complete it based on the current working directory.
  //
  repository_location
  parse_location (const char*);
}

#endif // BPKG_MANIFEST_UTILITY_HXX
