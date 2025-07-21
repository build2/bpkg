// file      : bpkg/fetch-cache.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_FETCH_CACHE_HXX
#define BPKG_FETCH_CACHE_HXX

#include <odb/sqlite/database.hxx>

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
  // `-- git/  -- git repositories in the fetched state
  //
  // ~/.build2/cache/
  // |
  // `-- src/  -- package source directories unpacked from archives or checked
  //              out (and distributed) from git repositories
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
  class fetch_cache
  {
    // Construction and open/close.
    //
  public:
    // Create an unopened object. The passed database should correspond to the
    // configuration on which the operation requiring the cache is being
    // performed. Make sure you don't reuse cache instanced across different
    // configurations. If there is no configuration (e.g., rep-info), then
    // pass NULL.
    //
    // Note that the object should only be opened if enabled() below returns
    // true.
    //
    fetch_cache (const common_options&, const database*);

    // Lock and open the fetch cache database.
    //
    // Issue diagnotics and throw failed if anything goes wrong. Issue
    // progress indication if waiting for the cache to become unlocked.
    //
    void
    open (tracer&);

    bool
    is_open () const
    {
      return db_ != nullptr;
    }

    void
    close ();

    ~fetch_cache ()
    {
      close ();
    }

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

    // @@ FC: should we drop it if still unused?
    //
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
    save_pkg_repository_auth (string id, string fingerprint, string name);

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
    load_pkg_repository_metadata (const repository_url&);

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
    save_pkg_repository_metadata (const repository_url&,
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
    path
    save_pkg_repository_package (package_id,
                                 version,
                                 path file,
                                 string checksum);

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
    // Transaction wrapper that allows starting a transaction and making it
    // current, for the duration of it's lifetime, in the presence of another
    // current transaction.
    //
    // Note that normally the cache functions will start the cache database
    // transactions when the caller has already started a configuration
    // database transaction.
    //
    class transaction
    {
    public:
      explicit
      transaction (odb::sqlite::transaction_impl* t)
          : t_ (), // Finalized.
            ct_ (nullptr)
      {
        using odb::sqlite::transaction;

        transaction* ct (transaction::has_current ()
                         ? &transaction::current ()
                         : nullptr);

        t_.reset (t, ct == nullptr);

        if (ct != nullptr)
          transaction::current (t_);

        ct_ = ct;
      }

      explicit
      transaction (odb::sqlite::database& db)
          : transaction (db.begin_exclusive ()) {}

      void
      commit ()
      {
        t_.commit ();
      }

      void
      rollback ()
      {
        t_.rollback ();
      }

      ~transaction ()
      {
        if (!t_.finalized ())
          t_.rollback ();

        if (ct_ != nullptr)
          odb::sqlite::transaction::current (*ct_);
      }

    private:
      odb::sqlite::transaction t_;
      odb::sqlite::transaction* ct_;
    };

    // Effective mode for this configuration.
    //
    bool enabled_;
    bool src_;
    bool trust_;

    void
    lock ();

    unique_ptr<odb::sqlite::database> lock_;
    unique_ptr<odb::sqlite::database> db_;
  };
}

#endif // BPKG_FETCH_CACHE_HXX
