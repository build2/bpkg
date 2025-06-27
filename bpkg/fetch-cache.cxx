// file      : bpkg/fetch-cache.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch-cache.hxx>

#include <odb/query.hxx>
#include <odb/result.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/sqlite/exceptions.hxx>

#include <bpkg/diagnostics.hxx>

#include <bpkg/fetch-cache-data.hxx>
#include <bpkg/fetch-cache-data-odb.hxx>

namespace bpkg
{
  using namespace odb::sqlite;
  using odb::schema_catalog;

  struct cache_mode
  {
    bool src = true;
    bool trust = true;
    bool offline = false;
  };

  static optional<cache_mode> mode_;

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

  static optional<bool> enabled_;

  bool fetch_cache::
  enabled (const common_options& co)
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

    return *enabled_;
  }

  bool fetch_cache::
  offline (const common_options& co)
  {
    return mode (co).offline;
  }

  bool fetch_cache::
  cache_src (const common_options& co)
  {
    return enabled (co) && mode (co).src;
  }

  bool fetch_cache::
  cache_trust (const common_options& co)
  {
    return enabled (co) && mode (co).trust;
  }

  static dir_path directory_;

  static const dir_path&
  directory (const common_options& co)
  {
    if (directory_.empty ())
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

    return directory_;
  }

  static string session_;

#if 0 // @@ TMP
  static const string&
  session (const common_options& co)
  {
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
    // case where they force-validate the cache while offline).
    //
    if (session_.empty ())
    {
      if (co.fetch_cache_session_specified ())
        session_ = co.fetch_cache_session ();
      else if (optional<string> v = getenv ("BPKG_FETCH_CACHE_SESSION"))
        session_ = move (*v);

      if (session_.empty ())
        session_ = uuid::generate ().string ();
    }

    return session_;
  }
#endif

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

  odb::sqlite::database
  open (const common_options& co, tracer& tr)
  {
    assert (fetch_cache::enabled (co));

    tracer trace ("fetch_cache");

    dir_path d (directory (co));
    path f (d / "fetch-cache.sqlite3");

    bool create (!exists (f));

    if (create)
      mk_p (d);

    using database = odb::sqlite::database;

    try
    {
      // We don't need the thread pool.
      //
      unique_ptr<connection_factory> cf (new single_connection_factory);

      database db (f.string (),
                   SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0),
                   true,                  // Enable FKs.
                   "",                    // Default VFS.
                   move (cf));

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
      return db;
    }
    catch (const database_exception& e)
    {
      fail << f << ": " << e.message () << endf;
    }
  }

  fetch_cache::
  fetch_cache (const common_options& co, tracer& tr)
      : db_ (open (co, tr))
  {
  }
}
