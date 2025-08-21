// file      : bpkg/fetch-cache.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_FETCH_CACHE_HXX
#define BPKG_FETCH_CACHE_HXX

#include <odb/sqlite/forward.hxx> // odb::sqlite::database

#include <libbpkg/manifest.hxx>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // database
#include <bpkg/utility.hxx>

#include <bpkg/diagnostics.hxx>
#include <bpkg/common-options.hxx>

#include <bpkg/package-common.hxx> // package_id

namespace bpkg
{
  // The local fetch cache is a singleton that is described by a bunch of
  // static variables (not exposed). The class itself serves as a RAII lock --
  // while an instance is alive, we have the cache database locked and open.
  //
  // The cache by default is split across two directories: ~/.cache/build2/
  // (or equivalent) for non-precious data (pkg/ and git/ subdirectories
  // below) and ~/.build2/cache/ for semi-precious data (src/ subdirectory
  // below). However, if the cache location is specified explicitly by the
  // user (--fetch-cache-path or BPKG_FETCH_CACHE_PATH), then both types of
  // data are placed into the specified directory.
  //
  // The cache database file is called fetch-cache.sqlite3 and can reside in
  // either location. Specifically, if we start operating with shared src
  // disabled (for example bpkg is used directly), then we place the database
  // file into ~/.cache/build2/. But as soon as we open the cache with shared
  // src enabled, we move the database to ~/.build2/cache/. The motivation for
  // this semantics is the fact that until we have shared source directories,
  // fetch-cache.sqlite3 is not precious. Plus, we don't want to create
  // ~/.build2/ until necessary (think the user only does package consumption
  // via bpkg). Note that there is also fetch-cache.lock that is always
  // created in ~/.cache/build2/ and which is used to protect agains races in
  // this logic (see the fetch_cache::open() implementation for details).
  //
  // The cache data is stored in the following subdirectories:
  //
  // ~/.cache/build2/
  // |
  // |-- pkg/  -- archive repositories metadata and package archives
  // |-- git/  -- git repositories in the fetched state
  // `-- tmp/  -- temporary directory for intermediate results
  //
  // ~/.build2/cache/
  // |
  // |-- src/  -- package source directories unpacked from archives or checked
  // |            out (and distributed) from git repositories
  // `-- tmp/  -- temporary directory for intermediate results
  //
  // The pkg/ subdirectory has the following structure:
  //
  // pkg/
  // |-- metadata/
  // |   `-- 1ecc6299db9ec823/
  // |       |-- packages.manifest
  // |       `-- repositories.manifest
  // `-- packages/
  //     `-- libfoo-1.2.3.tar.gz
  //
  // The git/ subdirectory has the following structure:
  //
  // git/
  // `-- 1ecc6299db9ec823/
  //     |-- repository/
  //     |   `-- .git/
  //     `-- ls-remote.txt
  //
  // The src/ subdirectory has the following structure:
  //
  // src/
  // `-- libfoo-1.2.3/
  //
  // The directories inside metadata/ are abbreviated SHA256 hashes of
  // repository URLs. Note that the signature.manifest files are not stored:
  // the signature is verified immediately after downloading and the checksum
  // is stored in the database.
  //
  // The package archive directory is shared among all the repositories,
  // meaning that if two repositories contain the same package version, we
  // will only store one archive (this makes sense considering that we can
  // only use one archive in any given build configuration). Currently we warn
  // if archive checksums don't match. In the future, once we have support for
  // reproducible source archives, we can consider upgrading this to an error.
  //
  // Note that inside the database we keep relative paths to filesystem
  // entries inside the cache. This allows the entire cache to be moved.
  //
  class fetch_cache
  {
    // Construction and open/close.
    //
  public:
    // Create an unopened object. The passed database should correspond to the
    // configuration on which the operation requiring the cache is being
    // performed. If there is no configuration (e.g., rep-info), then pass
    // NULL (can also be used to create an "uninitialized" instance that will
    // be initialized with the mode() call).
    //
    // Note that the object should only be opened if enabled() below returns
    // true.
    //
    // NOTE: don't reuse cache instances across different configurations
    // without prior mode(common_options, database*) function call.
    //
    fetch_cache (const common_options&, const database*);

    // Re-calculate the cache settings taking into account the configuration-
    // specific defaults, if the database is specified.
    //
    // NOTE: needs to be called before reusing the cache instance for a
    // different configuration or without configuration. Note also that this
    // way we may end up with a disabled but open fetch cache.
    //
    void
    mode (const common_options&, const database*);

    // Lock and open the fetch cache database.
    //
    // Issue diagnotics and throw failed if anything goes wrong. Issue
    // progress indication if waiting for the cache to become unlocked.
    //
    void
    open (tracer&);

    // Note: valid to call on an uninitialized instance.
    //
    bool
    is_open () const
    {
      return db_ != nullptr;
    }

    void
    close ();

    ~fetch_cache ();

    fetch_cache (fetch_cache&&) = delete;
    fetch_cache (const fetch_cache&) = delete;

    fetch_cache& operator= (fetch_cache&&) = delete;
    fetch_cache& operator= (const fetch_cache&) = delete;

    // Cache settings.
    //
  public:
    // Return true if the fetch caching is not diabled (--no-fetch-cache).
    //
    bool
    enabled () const;

    // Return true if we are in the offline mode (--offline).
    //
    // Note that we must respect it even if caching is disabled: while we
    // don't allow specifying --offline with --no-fetch-cache, caching can
    // also be disabled via BPKG_FETCH_CACHE=0.
    //
    bool
    offline () const;

    static bool
    offline (const common_options&);

    // Return true if fetch caching is enabled and sharing of source
    // directories for dependencies is not disabled (--fetch-cache=no-src).
    //
    bool
    cache_src () const;

    // Return true if fetch caching is enabled and caching of repository
    // authentication answers is not disabled (--fetch-cache=no-trust).
    //
    bool
    cache_trust () const;

    // Garbage collection.
    //
  public:
    // Start/stop removal of outdated cache entries. The cache is expected to
    // remain open between the calls to these functions. Note that no
    // load/save_*() functions (see below) can be called while the garbage
    // collection is in progress. Note also that close() will stop garbage
    // collection, if necessary, ignoring any errors.
    //
    // Normally, you would call start_gc() immediately before performing an
    // operation that takes long to complete (compared to removing a
    // filesystem entry), such as a network transfer, and then would call
    // stop_gc() immediately after. Typically, the start_gc()/stop_gc() calls
    // are nested between load/save_*() calls.
    //
    void
    start_gc ();

    // Unless ignore_errors is true, issue diagnostics and throw failed if
    // there was an error during garbage collection.
    //
    void
    stop_gc (bool ignore_errors = false);

    // Return true if garbage collection has been started but hasn't yet
    // been stopped.
    //
    bool
    active_gc () const
    {
      return gc_thread_.joinable ();
    }

    // Trusted (authenticated) pkg repository certificates cache API.
    //
    // Note that the load_*() and save_*() functions should be called without
    // unlocking the cache in between (this could easily be relaxed, however,
    // currently these two functions are called inside the
    // load/save_pkg_repository_metadata() calls).
    //
  public:
    bool
    load_pkg_repository_auth (const string& id);

    void
    save_pkg_repository_auth (string id,
                              string fingerprint,
                              string name,
                              optional<timestamp> end_date);

    // Metadata cache API for pkg repositories.
    //
    // Note that the load_*() and save_*() functions should be called without
    // unlocking the cache in between.
    //
  public:
    // Load (find) metadata for the specified pkg repository URL.
    //
    // If returned *_checksum members are not empty, then an up-to-date check
    // is necessary.
    //
    struct loaded_pkg_repository_metadata
    {
      path   repositories_path;
      string repositories_checksum;

      path   packages_path;
      string packages_checksum;
    };

    optional<loaded_pkg_repository_metadata>
    load_pkg_repository_metadata (repository_url);

    // Save (insert of update) metadata for the specified pkg repository
    // URL. The metadata should be written to the returned paths. Note that
    // the caller is expected to use the "write to temporary and atomically
    // move into place" technique.
    //
    // If repositories_checksum is empty, then repositories.manifest file
    // need not be updated. In this case, repositories_path will be empty
    // as well.
    //
    struct saved_pkg_repository_metadata
    {
      path repositories_path;
      path packages_path;
    };

    saved_pkg_repository_metadata
    save_pkg_repository_metadata (repository_url,
                                  string repositories_checksum,
                                  string packages_checksum);

    // Package cache API for pkg repositories.
    //
    // Note that the load_*() and save_*() functions should be called without
    // unlocking the cache in between.
    //
  public:
    // Load (find) package archive for the specified package name and version.
    //
    struct loaded_pkg_repository_package
    {
      path archive;
      string checksum;
      repository_url repository;
    };

    optional<loaded_pkg_repository_package>
    load_pkg_repository_package (const package_id&);

    // Save (insert) package archive with the specified file name for the
    // specified package name and version. The archive should be placed
    // (copied, moved, hard-linked) to the returned path. Note that the caller
    // is expected to use the "place to temporary and atomically move into
    // place" technique.
    //
    // @@ FC Won't it be cleaner to just pass the path to the file and save*()
    //    does all the moving, etc? Maybe we don't keep the archive in the
    //    configuration if shared src is enable? Could probably pass a flag
    //    whether to move or copy/link...
    //
    path
    save_pkg_repository_package (package_id,
                                 version orig_version,
                                 path file,
                                 string checksum,
                                 repository_url);

    // State cache API for git repositories.
    //
    // Note that the load_*() and save_*() functions should be called without
    // unlocking the cache in between.
    //
  public:
    // Load (find) repository state for the specified git repository URL.
    //
    // Note that the returned paths point into the temporary directory and
    // which will be moved back into their permanent location by save_*().
    // This, in particular, means that save_*() should be called even if
    // nothing was fetched. If the cache entry is absent, the returned paths
    // are valid but the corresponding filesystem entries do not exist (but
    // their containing directory does). Likewise, if the cache entry is
    // outdated, then the returned ls-remote output path is valid but the
    // corresponding filesystem entry does not exist.
    //
    struct loaded_git_repository_state
    {
      enum state_type
      {
        absent,    // No cache entry for this repository yet.
        outdated,  // Existing cache entry but ls-remote output is out of date.
        up_to_date // Existing cache entry and ls-remote output is up to date.
      };

      dir_path repository;
      path ls_remote;
      state_type state;
    };

    loaded_git_repository_state
    load_git_repository_state (repository_url);

    // Save (insert of update) repository state for the specified git
    // repository URL. Specifically, move the filesystem entries from the
    // paths returned by load_*() to their permanent location.
    //
    // Note that it's valid to call save_*() with absent ls-remote file. This
    // can be used to preserve (expensive to fetch) git repository state in
    // case of network failures during git-ls-remote (or, more generally,
    // before spoiling the git repository state). This can also be the case if
    // git-ls-remote call has not been made since there were no need to
    // resolve git references to commit ids.
    //
    // Also note that it's valid to not call save_*() after the load_*() call,
    // which indicates that the repository state is spoiled. In this case, the
    // repository temporary directory is removed on the next open() call.
    //
    void
    save_git_repository_state (repository_url);

    // Git repository state directory. The caching is expected to be enabled.
    //
    // Note that a repository state should never be amended via this path.
    // Normally, it is used as a global identifier of the repository cached
    // state (map key, etc).
    //
    dir_path
    git_repository_state_dir (repository_url) const;

    // Shared package source directory cache API.
    //
    // Note that the load_*() and save_*() functions should be called without
    // unlocking the cache in between.
    //
  public:
    // If the cache entry is present, then return the permanent source
    // directory path. Otherwise return the temporary directory path which
    // does not exist (but its containing directory does).
    //
    struct loaded_shared_source_directory_state
    {
      bool present;
      dir_path directory;
    };

    loaded_shared_source_directory_state
    load_shared_source_directory (const package_id&,
                                  const version& orig_version);

    // Given the filled temporary directory path, add the cache entry and
    // return the permanent source directory path.
    //
    dir_path
    save_shared_source_directory (package_id,
                                  version orig_version,
                                  dir_path tmp_directory,
                                  repository_url,
                                  string origin_id);

    // If the cache entry is present for the specified package, then return
    // its directory path and use count.
    //
    struct shared_source_directory_tracking
    {
      dir_path directory;
      uint64_t use_count;
    };

    optional<shared_source_directory_usage>
    load_shared_source_directory_tracking (const package_id&);

    // Start tracking the use of the shared source directory for the specified
    // package by the newly configured configuration directory. The
    // configuration directory path is expected to be absolute and
    // normalized. The use count should be as retrieved on the previous
    // load_shared_source_directory_tracked() call. Assume that the package
    // was configured using the configure `hardlink` parameter.
    //
    void
    save_shared_source_directory_tracking (const package_id&,
                                           const dir_path& configuration,
                                           uint64_t use_count);

    // Implementation details (also used by cfg_create()).
    //
  public:
    static optional<bool>
    enabled (const common_options&);

    struct cache_mode
    {
      optional<bool> src;
      optional<bool> trust;
      optional<bool> offline;
    };

    static cache_mode
    mode (const common_options&);

  private:
    class transaction;

    // Effective mode for this configuration.
    //
    bool enabled_;
    bool src_;
    bool trust_;

    // Database and its lock.
    //
    void
    lock ();

    unique_ptr<odb::sqlite::database> lock_;
    unique_ptr<odb::sqlite::database> db_;

    // Garbage collection.
    //
    void
    garbage_collector ();

    thread       gc_thread_;
    atomic<bool> gc_stop_;
    diag_record  gc_error_;
  };
}

#endif // BPKG_FETCH_CACHE_HXX
