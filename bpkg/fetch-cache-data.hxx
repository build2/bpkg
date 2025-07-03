// file      : bpkg/fetch-cache-data.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_FETCH_CACHE_DATA_HXX
#define BPKG_FETCH_CACHE_DATA_HXX

#include <odb/core.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

// Used by the data migration entries.
//
// NOTE: drop all the `#pragma db member(...) default(...)` pragmas when
//       migration is no longer supported (i.e., the current and base schema
//       versions are the same).
//
#define FETCH_CACHE_SCHEMA_VERSION_BASE 1

#pragma db model version(FETCH_CACHE_SCHEMA_VERSION_BASE, 1, open) // @@ TMP: close

namespace bpkg
{
  // Cache entry for metadata of pkg type repositories.
  //
  #pragma db object pointer(unique_ptr)
  class pkg_repository_metadata
  {
  public:
    // Repository URL.
    //
    // @@ Do we canonicalize local URLs to something uniform?
    //
    repository_url url;

    // Directory for this repository inside the metadata/ directory.
    // Calculated as a 16-character abbreviated SHA256 checksum of the
    // repository URL.
    //
    dir_path directory;

    // Session during which we last performed the up-to-date check of the
    // metadata.
    //
    string session;

    // Timestamp of the last time this cached entry was accessed.
    //
    timestamp access_time;

    // The repositories.manifest file path inside the repository directory and
    // its SHA256 checksum as recorded in the packages.manifest file header.
    //
    path   repositories_path;
    string repositories_checksum;

    // The packages.manifest file path inside the repository directory and
    // its SHA256 checksum as recorded in the signature.manifest file.
    //
    path   packages_path;
    string packages_checksum;

    // Database mapping.
    //
    #pragma db member(url) id
    #pragma db member(dir) unique
  };
}

#endif // BPKG_FETCH_CACHE_DATA_HXX
