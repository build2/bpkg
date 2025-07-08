// file      : bpkg/fetch-cache.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_FETCH_CACHE_HXX
#define BPKG_FETCH_CACHE_HXX

#include <odb/sqlite/database.hxx>

#include <libbpkg/manifest.hxx>

#include <bpkg/types.hxx>
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
  // The cache by default resides in ~/.build2/cache/. The cache database
  // file is called fetch-cache.sqlite3. The cache data is stored in the
  // following subdirectories next to it:
  //
  // ~/.build2/cache/
  // |
  // |-- pkg/  -- archive repositories metadata and package archives
  // |-- git/  -- git repositories in the fetched state
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
    // Create an unopened object.
    //
    explicit
    fetch_cache (const common_options&);

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
    };

    optional<loaded_pkg_repository_package>
    load_pkg_repository_package (const package_id&);

    // Save (insert) package archive for the specified package name and
    // version. The archive should be placed (copied, moved, hard-linked) to
    // the returned path. Note that the caller is expected to use the "place
    // to temporary and atomically move into place" technique.
    //
    path
    save_pkg_repository_package (package_id, version, string checksum);

  private:
    using database = odb::sqlite::database;

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
      transaction (database& db): transaction (db.begin_exclusive ()) {}

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

    unique_ptr<database> db_;
  };
}

#endif // BPKG_FETCH_CACHE_HXX
