// file      : bpkg/fetch-cache-data.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_FETCH_CACHE_DATA_HXX
#define BPKG_FETCH_CACHE_DATA_HXX

#include <odb/core.hxx>
#include <odb/section.hxx>

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
    optional_timestamp end_date; // notAfter (UTC, absent if dummy).

    // Database mapping.
    //
    #pragma db member(id) id
  };

  #pragma db view object(pkg_repository_auth)
  struct pkg_repository_auth_count
  {
    #pragma db column("count(*)")
    size_t result;

    operator size_t () const {return result;}
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
    // file:/foo
    //
    // If local, then on Windows it is canonicalized by converting its path
    // into lower case. Note that such a canonicalization is consistent with
    // the repository location canonical name production.
    //
    repository_url url;

    // Directory for this repository inside the metadata/ directory.
    // Calculated as a 16-character abbreviated SHA256 checksum of the
    // canonicalized repository URL.
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

    // Speed-up queries with filtering by the access time.
    //
    #pragma db member(access_time) index
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

    // The package archive file path inside the packages/ directory, its
    // SHA256 checksum as recorded in the packages.manifest file (which should
    // match the actual contents checksum), and its origin repository.
    //
    path archive;
    string checksum;
    repository_url repository;

    // Database mapping.
    //
    #pragma db member(id) id column("")
    #pragma db member(version) set(this.version.init (this.id.version, (?)))
    #pragma db member(archive) unique

    // Speed-up queries with filtering by the access time.
    //
    #pragma db member(access_time) index
  };

  // Cache entry for state of git type repositories.
  //
  #pragma db object pointer(unique_ptr)
  class git_repository_state
  {
  public:
    // Repository URL.
    //
    // May not contain fragment. For local URLs may not be a relative path.
    //
    // Note that the following local URLs end up with the same /foo.git string
    // representation:
    //
    // /foo.git
    // file:///foo.git
    // file://localhost/foo.git
    // file:/foo.git
    //
    // Canonicalized as follows:
    //
    // - If local, then on Windows convert its path into lower case.
    // - Strip the .git extension, if present, from its path.
    //
    // Note that such a canonicalization is consistent with the repository
    // location canonical name production.
    //
    repository_url url;

    // Directory for this repository inside the git/ directory. Calculated as
    // a 16-character abbreviated SHA256 checksum of the canonicalized
    // repository URL.
    //
    dir_path directory;

    // Session during which we last performed git-ls-remote.
    //
    string session;

    // Timestamp of the last time this cached entry was accessed.
    //
    timestamp access_time;

    // Database mapping.
    //
    #pragma db member(url) id
    #pragma db member(directory) unique

    // Speed-up queries with filtering by the access time.
    //
    #pragma db member(access_time) index
  };

  // Cache entry for shared package source directory.
  //
  #pragma db object pointer(unique_ptr)
  class shared_source_directory
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

    // Directory for this package inside the src/ directory.
    //
    dir_path directory;

    // The origin of this package. For package archive the origin id is its
    // SHA256 checksum as recorded in the packages.manifest file (which should
    // match the actual contents checksum). For a git repository checkout it
    // is the commit id. These are kept primarily for debuggability.
    //
    repository_url repository;
    string         origin_id;

    // Path to src-root.build[2] file inside the shared source directory.
    // Keeps track of the shared source directory usage by package
    // configurations on the same filesystem, as this file's hard link count
    // (see b-configure hardlink parameter for details).
    //
    // Note that this file doesn't exist initially and is only created by
    // pkg-configure executed in configuration on the same filesystem.
    //
    path src_root;

    // List of package configurations, represented by their src-root.build[2]
    // file paths, located on filesystems other than the one of the shared
    // source directory they refer to.
    //
    // Note that complementing src_root by this list doesn't result in a
    // bullet-proof use counting (think of configuration renames, etc), but is
    // probably the best approximation we can get without heroic measures.
    //
    paths configurations;
    odb::section configurations_section;

    shared_source_directory () = default;

    shared_source_directory (package_id i,
                             original_version v,
                             timestamp a,
                             dir_path d,
                             repository_url r,
                             string o,
                             path s)
        : id (move (i)),
          version (move (v)),
          access_time (a),
          directory (move (d)),
          repository (move (r)),
          origin_id (move (o)),
          src_root (move (s)) {}

    // Database mapping.
    //
    #pragma db member(id) id column("")
    #pragma db member(version) set(this.version.init (this.id.version, (?)))
    #pragma db member(directory) unique

    #pragma db member(configurations) id_column("") value_column("src_root") \
      section(configurations_section)

    #pragma db member(configurations_section) load(lazy) update(always)

    // Speed-up queries with filtering by the access time.
    //
    #pragma db member(access_time) index
  };
}

#endif // BPKG_FETCH_CACHE_DATA_HXX
