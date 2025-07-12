// file      : bpkg/fetch-cache-data.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_FETCH_CACHE_DATA_HXX
#define BPKG_FETCH_CACHE_DATA_HXX

#include <odb/core.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

// Must be included last and have no <libbpkg/manifest.hxx> inclusion in front
// of it (includes it itself; see assert and _version in package-common.hxx
// for details).
//
#include <bpkg/package-common.hxx>

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
  // Cache entry for trusted (authenticated) pkg repository certificates.
  //
  // See the certificate class in package.hxx for background.
  //
  #pragma db object pointer(unique_ptr)
  class pkg_repository_auth
  {
  public:
    // Note that we only keep a minimum subset of data compared to what is
    // stored in the certificate class since whenever the cache is consulted,
    // the caller should have access to the full certificate. We don't even
    // need to store fingerprint and name, but let's keep them for
    // debuggability.
    //
    // Note that the cache includes entries for dummy certificates
    // corresponding to unsigned repositories.
    //
    string id;          // SHA256 fingerprint truncated to 16 characters.
    string fingerprint; // Fingerprint canonical representation (empty if dummy).
    string name;        // CN component of Subject.

    // Database mapping.
    //
    #pragma db member(id) id
  };

  // Cache entry for metadata of pkg type repositories.
  //
  #pragma db object pointer(unique_ptr)
  class pkg_repository_metadata
  {
  public:
    // Repository URL.
    //
    // May not contain fragment. For local URLs may not be a relative path.
    //
    // Note that the following local URLs end up with the same /foo string
    // representation:
    //
    // /foo
    // file:///foo
    // file://localhost/foo
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
    #pragma db member(directory) unique
  };

  // Cache entry for package archive of pkg type repositories.
  //
  #pragma db object pointer(unique_ptr)
  class pkg_repository_package
  {
  public:
    // Note that currently we don't really need the original version, but
    // let's keep it if that changes in the future and for debuggability.
    //
    package_id id;
    original_version version;

    // Timestamp of the last time this cached entry was accessed.
    //
    timestamp access_time;

    // The package archive file path inside the packages/ directory and its
    // SHA256 checksum as recorded in the packages.manifest file (which should
    // match the actual contents checksum).
    //
    path archive;
    string checksum;

    // Database mapping.
    //
    #pragma db member(id) id column("")
    #pragma db member(version) set(this.version.init (this.id.version, (?)))
    #pragma db member(archive) unique
  };

  #pragma db view object(pkg_repository_auth)
  struct pkg_repository_auth_count
  {
    #pragma db column("count(*)")
    size_t result;

    operator size_t () const {return result;}
  };
}

#endif // BPKG_FETCH_CACHE_DATA_HXX
