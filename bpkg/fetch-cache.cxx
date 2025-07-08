// file      : bpkg/fetch-cache.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch-cache.hxx>

#include <odb/query.hxx>
#include <odb/result.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/sqlite/exceptions.hxx>

#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx> // {repositories,packages}_file

#include <bpkg/fetch-cache-data.hxx>
#include <bpkg/fetch-cache-data-odb.hxx>

namespace bpkg
{
  using namespace odb::sqlite;
  using odb::schema_catalog;

  using butl::system_clock;

  // Note that directory and session are only initialized if the cache is
  // enabled.
  //
  struct cache_mode
  {
    bool src = true;
    bool trust = true;
    bool offline = false;
  };

  static optional<bool> enabled_;
  static optional<cache_mode> mode_;
  static dir_path directory_;
  static string session_;

  static const cache_mode&
  mode (const common_options& co)
  {
    if (!mode_)
    {
      mode_ = cache_mode ();

      auto parse = [] (const string& s, const char* what)
      {
        for (size_t b (0), e (0), n; (n = next_word (s, b, e, ',')) != 0; )
        {
          if      (s.compare (b, n, "no-src") == 0)   mode_->src = false;
          else if (s.compare (b, n, "no-trust") == 0) mode_->trust = false;
          else if (s.compare (b, n, "offline") == 0)  mode_->offline = true;
          else
          {
            // Ideally this should be detected earlier, but better late than
            // never.
            //
            fail << "invalid " << what << " value '" << string (s, b, n) << "'";
          }
        }
      };

      if (optional<string> v = getenv ("BPKG_FETCH_CACHE"))
      {
        if (*v != "0" && *v != "false")
          parse (*v, "BPKG_FETCH_CACHE environment variable");
      }

      if (co.fetch_cache_specified ())
        parse (co.fetch_cache (), "--fetch-cache option");

      if (co.offline ())
        mode_->offline = true;
    }

    return *mode_;
  }

  fetch_cache::
  fetch_cache (const common_options& co)
  {
    if (!enabled_)
    {
      if (co.no_fetch_cache ())
        enabled_ = false;
      else if (optional<string> v = getenv ("BPKG_FETCH_CACHE"))
      {
        if (*v == "0" || *v == "false")
          enabled_ = false;
      }

      if (!enabled_)
        enabled_ = true;
    }

    // Initialize mode for non-static accessors below. We have to do it
    // regardless of whether the cache is enabled due to offline().
    //
    mode (co);

    // Get specified or calculate default cache directory.
    //
    if (*enabled_ && directory_.empty ())
    {
      const char* w (nullptr);
      try
      {
        if (co.fetch_cache_path_specified ())
        {
          w = "--fetch-cache-path option";

          directory_ = co.fetch_cache_path ();
        }
        else if (optional<string> v = getenv ("BPKG_FETCH_CACHE_PATH"))
        {
          w = "BPKG_FETCH_CACHE_PATH environment variable";

          directory_ = dir_path (move (*v));
        }

        if (directory_.empty ())
        {
          try
          {
            w = "user's home directory";

            directory_ = path::home_directory ();
            directory_ /= ".build2";
            directory_ /= "cache";
          }
          catch (const system_error&)
          {
            fail << "unable to obtain user's home directory to derive "
                 << "local fetch cache path" <<
              info << "use --fetch-cache-path option or BPKG_FETCH_CACHE_PATH "
                   << "environment variable to specify explicitly" <<
              info << "use --no-fetch-cache to disable caching";
          }
        }
      }
      catch (const invalid_path& e)
      {
        fail << "invalid local fetch cache path '" << e.path << "'" <<
          info << "derived from " << w;
      }
    }

    // Get specified or generate new fetch cache session id.
    //
    // Note that we shouldn't be rechecking up-to-dateness of the same
    // repository metadata in a single bpkg invocation (but we could re-fetch
    // the same package, for example, into a linked configuration). However,
    // let's generate the session id anyway, in case this changes (or we start
    // using the session id for packages).
    //
    // Note also that a session doesn't really make sense when working offline
    // (we don't do up-to-date checks anyway). But let's keep it the same as
    // the online case for simplicity (plus someone could come up with a use-
    // case where they want force-validate the cache by fetching offline).
    //
    if (*enabled_ && session_.empty ())
    {
      if (co.fetch_cache_session_specified ())
        session_ = co.fetch_cache_session ();
      else if (optional<string> v = getenv ("BPKG_FETCH_CACHE_SESSION"))
        session_ = move (*v);

      if (session_.empty ())
        session_ = uuid::generate ().string ();
    }
  }

  static const string schema_name ("fetch-cache"); // Database schema name.

  // Register the data migration functions.
  //
#if 0
  template <odb::schema_version v>
  using migration_entry = odb::data_migration_entry<v, FETCH_CACHE_SCHEMA_VERSION_BASE>;

  static const migration_entry<2>
  migrate_v2 ([] (odb::database& db)
  {
  },
  schema_name);
#endif

  void fetch_cache::
  open (tracer& tr)
  {
    assert (enabled () && !is_open ());

    tracer trace ("fetch_cache::open");

    const dir_path& d (directory_);
    path f (d / "fetch-cache.sqlite3");

    bool create (!exists (f));

    if (create)
      mk_p (d);

    try
    {
      // We don't need the thread pool.
      //
      unique_ptr<connection_factory> cf (new single_connection_factory);

      db_.reset (
        new database (
          f.string (),
          SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0),
          true,                  // Enable FKs.
          "",                    // Default VFS.
          move (cf)));

      database& db (*db_);

      db.tracer (trace);

      // Lock the database for as long as the connection is active. First we
      // set locking_mode to EXCLUSIVE which instructs SQLite not to release
      // any locks until the connection is closed. Then we force SQLite to
      // acquire the write lock by starting exclusive transaction. See the
      // locking_mode pragma documentation for details. This will also fail if
      // the database is inaccessible (e.g., file does not exist, already used
      // by another process, etc).
      //
      try
      {
        connection_ptr c (db.connection ());
        c->execute ("PRAGMA locking_mode = EXCLUSIVE");
        transaction t (c->begin_exclusive ());

        const string& sn (schema_name);

        if (create)
        {
          // Create the new schema.
          //
          if (db.schema_version (sn) != 0)
            fail << f << ": already has database schema";

          schema_catalog::create_schema (db, sn);

          // @@ TODO: also recreate all the directories where we store the
          // data.
        }
        else
        {
          // Migrate the database if necessary.
          //
          odb::schema_version sv  (db.schema_version (sn));
          odb::schema_version scv (schema_catalog::current_version (db, sn));

          if (sv != scv)
          {
            if (sv < schema_catalog::base_version (db, sn))
              fail << "local fetch cache " << f << " is too old";

            if (sv > scv)
              fail << "local fetch cache " << f << " is too new";

            schema_catalog::migrate (db, scv, sn);
          }
        }

        t.commit ();
      }
      catch (odb::timeout&)
      {
        // @@ TODO: sleep and retry, also issue progress diagnostics.
        //
        // Maybe we should first wait up to 200ms before issuing progress?

        fail << "fetch cache " << f << " is already used by another process";
      }

      db.tracer (tr); // Switch to the caller's tracer.
    }
    catch (const database_exception& e)
    {
      fail << f << ": " << e.message () << endf;
    }
  }

  void fetch_cache::
  close ()
  {
    if (is_open ())
    {
      db_.reset ();
    }
  }

  bool fetch_cache::
  enabled () const
  {
    return *enabled_;
  }

  bool fetch_cache::
  offline () const
  {
    return mode_->offline;
  }

  bool fetch_cache::
  offline (const common_options& co)
  {
    return mode (co).offline;
  }

  bool fetch_cache::
  cache_src () const
  {
    return *enabled_ && mode_->src;
  }

  bool fetch_cache::
  cache_trust () const
  {
    return *enabled_ && mode_->trust;
  }

  bool fetch_cache::
  load_pkg_repository_auth (const string& id)
  {
    assert (is_open ()); // The open() function should have been called.

    database& db (*db_);
    transaction t (db);

    bool r (db.query_value<pkg_repository_auth_count> (
              query<pkg_repository_auth_count>::id == id) != 0);

    t.commit ();
    return r;
  }

  void fetch_cache::
  save_pkg_repository_auth (string id, string fingerprint, string name)
  {
    // The load_pkg_repository_auth() function should have been called.
    //
    assert (is_open ());

    database& db (*db_);
    transaction t (db);

    pkg_repository_auth a {move (id), move (fingerprint), move (name)};
    db.persist (a);

    t.commit ();
  }

  static dir_path pkg_repository_metadata_directory_;

  optional<fetch_cache::loaded_pkg_repository_metadata> fetch_cache::
  load_pkg_repository_metadata (const repository_url& u)
  {
    // The overall plan is as follows:
    //
    // 1. See if there is an entry for this URL in the database. If not,
    //    return nullopt.
    //
    // 2. Check if filesystem entries for this cache entry are present on
    //    disk. If not, remove the entry from the database, remove the
    //    metadata directory on disk, and return nullopt.
    //
    // 3. Unless offline, if the current session doesn't match entry session,
    //    then return checksums to indicate an up-to-date check is necessary.
    //
    // 4. Update entry session and access_time.
    //
    // 5. Return paths and checksums.

    optional<loaded_pkg_repository_metadata> r;

    if (pkg_repository_metadata_directory_.empty ())
    {
      pkg_repository_metadata_directory_ =
        ((dir_path (directory_) /= "pkg") /= "metadata");
    }

    assert (is_open ()); // The open() function should have been called.

    database& db (*db_);
    transaction t (db);

    pkg_repository_metadata m;
    if (db.find<pkg_repository_metadata> (u, m))
    {
      dir_path d (pkg_repository_metadata_directory_ / m.directory);

      path rf (d / m.repositories_path);
      path pf (d / m.packages_path);

      if (!exists (rf) || !exists (pf))
      {
        // Remove the database entry last, to make sure we are still tracking
        // the directory if its removal fails for any reason.
        //
        rm_r (d);
        db.erase (m);
      }
      else
      {
        bool utd (!offline () && m.session != session_); // Up-to-date check.

        m.session = session_;
        m.access_time = system_clock::now ();

        db.update (m);

        r = loaded_pkg_repository_metadata {
          move (rf),
          utd ? move (m.repositories_checksum) : string (),
          move (pf),
          utd ? move (m.packages_checksum) : string ()};
      }
    }

    t.commit ();

    return r;
  }

  fetch_cache::saved_pkg_repository_metadata fetch_cache::
  save_pkg_repository_metadata (const repository_url& u,
                                string repositories_checksum,
                                string packages_checksum)
  {
    // The overall plan is as follows:
    //
    // 1. Try to load the current entry from the database:
    //
    //    a. If present, update checksums and remove files to be updated.
    //
    //    b. If absent, then assert repositories_checksum is specified and
    //       recreate the metadata directory on disk. Create new database
    //       entry with current session and access time.
    //
    // 2. Return the paths the metadata should be written to.

    // The load_pkg_repository_metadata() function should have been called.
    //
    assert (!pkg_repository_metadata_directory_.empty ());

    // Metadata file paths.
    //
    path rf;
    path pf;

    database& db (*db_);
    transaction t (db);

    pkg_repository_metadata m;
    if (db.find<pkg_repository_metadata> (u, m))
    {
      dir_path d (pkg_repository_metadata_directory_ / m.directory);

      if (!repositories_checksum.empty ())
      {
        m.repositories_checksum = move (repositories_checksum);

        rf = d / m.repositories_path;
        rm (rf);
      }

      m.packages_checksum = move (packages_checksum);

      pf = d / m.packages_path;
      rm (pf);

      db.update (m);
    }
    else
    {
      assert (!repositories_checksum.empty ()); // Shouldn't be here otherwise.

      dir_path dn (sha256 (u.string ()).abbreviated_string (16));
      dir_path d (pkg_repository_metadata_directory_ / dn);

      // If the metadata directory already exists, probably as a result of
      // some previous failure, then re-create it.
      //
      if (exists (d))
        rm_r (d);

      mk_p (d);

      rf = d / repositories_file;
      pf = d / packages_file;

      pkg_repository_metadata md {
        u,
        move (dn),
        session_,
        system_clock::now (),
        repositories_file,
        move (repositories_checksum),
        packages_file,
        move (packages_checksum)};

      db.persist (md);
    }

    t.commit ();

    return saved_pkg_repository_metadata {move (rf), move (pf)};
  }

  static dir_path pkg_repository_package_directory_;

  optional<fetch_cache::loaded_pkg_repository_package> fetch_cache::
  load_pkg_repository_package (const package_id& id)
  {
    // The overall plan is as follows:
    //
    // 1. See if there is an entry for this package id in the database. If
    //    not, return nullopt.
    //
    // 2. Check if the archive file is present for this cache entry. If not,
    //    remove the entry from the database and return nullopt.
    //
    // 3. Update entry access_time.
    //
    // 4. Return the archive path and checksum.

    optional<loaded_pkg_repository_package> r;

    if (pkg_repository_package_directory_.empty ())
    {
      pkg_repository_package_directory_ =
        ((dir_path (directory_) /= "pkg") /= "packages");
    }

    assert (is_open ()); // The open() function should have been called.

    database& db (*db_);
    transaction t (db);

    pkg_repository_package p;
    if (db.find<pkg_repository_package> (id, p))
    {
      path f (pkg_repository_package_directory_ / p.archive);

      if (!exists (f))
      {
        db.erase (p);
      }
      else
      {
        p.access_time = system_clock::now ();

        db.update (p);

        r = loaded_pkg_repository_package {move (f), move (p.checksum)};
      }
    }

    t.commit ();

    return r;
  }

  path fetch_cache::
  save_pkg_repository_package (package_id id, version v, string checksum)
  {
    // The overall plan is as follows:
    //
    // 1. Create new database entry with current access time. Remove the
    //    archive file, if exists.
    //
    // 2. Return the path the archive file should be moved to.

    // The load_pkg_repository_package() function should have been called.
    //
    assert (!pkg_repository_package_directory_.empty ());

    if (!exists (pkg_repository_package_directory_))
      mk_p (pkg_repository_package_directory_);

    path an (id.name.string () + '-' + v.string () + ".tar.gz");
    path r (pkg_repository_package_directory_ / an);

    database& db (*db_);
    transaction t (db);

    // If the archive file already exists, probably as a result of some
    // previous failure, then remove it.
    //
    if (exists (r))
      rm (r);

    pkg_repository_package p {
      move (id),
      move (v),
      system_clock::now (),
      move (an),
      move (checksum)};

    db.persist (p);

    t.commit ();
    return r;
  }
}
