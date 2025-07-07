// file      : bpkg/fetch-cache-data.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_FETCH_CACHE_DATA_HXX
#define BPKG_FETCH_CACHE_DATA_HXX

#include <chrono>
#include <type_traits> // static_assert

#include <odb/core.hxx>

#include <libbutl/timestamp.hxx>

#include <libbpkg/manifest.hxx>

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
  // timestamp
  //
  using butl::timestamp;
  using butl::timestamp_unknown;

  // Ensure that timestamp can be represented in nonoseconds without loss of
  // accuracy, so the following ODB mapping is adequate.
  //
  static_assert (
    std::ratio_greater_equal<timestamp::period,
                             std::chrono::nanoseconds::period>::value,
    "The following timestamp ODB mapping is invalid");

  // As pointed out in libbutl/timestamp.hxx we will overflow in year 2262, but
  // by that time some larger basic type will be available for mapping.
  //
  #pragma db map type(timestamp) as(uint64_t)                 \
    to(std::chrono::duration_cast<std::chrono::nanoseconds> ( \
         (?).time_since_epoch ()).count ())                   \
    from(butl::timestamp (                                    \
      std::chrono::duration_cast<butl::timestamp::duration> ( \
        std::chrono::nanoseconds (?))))

  // path
  //
  // In some contexts it may denote directory, so lets preserve the trailing
  // slash, if present.
  //
  #pragma db map type(path) as(string)  \
    to((?).representation ()) from(bpkg::path (?))

  #pragma db map type(dir_path) as(string)  \
    to((?).string ()) from(bpkg::dir_path (?))

  // repository_url
  //
  #pragma db value(repository_url) type("TEXT")

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
}

#endif // BPKG_FETCH_CACHE_DATA_HXX
