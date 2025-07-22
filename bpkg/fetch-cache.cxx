// file      : bpkg/fetch-cache.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch-cache.hxx>

#include <odb/query.hxx>
#include <odb/result.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/sqlite/exceptions.hxx>

#include <bpkg/database.hxx>         // database::fetch_cache_mode
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx> // {repositories,packages}_file

#include <bpkg/fetch-cache-data.hxx>
#include <bpkg/fetch-cache-data-odb.hxx>

namespace bpkg
{
  using namespace odb::sqlite;
  using odb::schema_catalog;

  using butl::system_clock;

  namespace chrono = std::chrono;

  // Note that directory and session are only initialized if the cache is
  // enabled. The semi-precious directory is left empty if it's the same as
  // non-precious.
  //
  using cache_mode = fetch_cache::cache_mode;

  static optional<optional<bool>> ops_enabled_;
  static optional<cache_mode> ops_mode_;
  static dir_path np_directory_; // Non-precious  (~/.cache/build2/).
  static dir_path sp_directory_; // Semi-precious (~/.build2/cache/).
  static string session_;

  cache_mode fetch_cache::
  mode (const common_options& co)
  {
    cache_mode r;

    auto parse = [&r] (const string& s, const char* what)
    {
      // NOTE: see also a special version of this below as well as in bdep.
      //
      for (size_t b (0), e (0), n; (n = next_word (s, b, e, ',')) != 0; )
      {
        if      (s.compare (b, n, "src") == 0)      r.src = true;
        else if (s.compare (b, n, "no-src") == 0)   r.src = false;
        else if (s.compare (b, n, "trust") == 0)    r.trust = true;
        else if (s.compare (b, n, "no-trust") == 0) r.trust = false;
        else if (s.compare (b, n, "offline") == 0)  r.offline = true;
        else
        {
          // Ideally this should be detected earlier, but better late than
          // never.
          //
          fail << "invalid " << what << " value '" << string (s, b, n) << "'";
        }
      }
    };

    // One can argue that the environment variable should be, priority-wise,
    // between the default options file and the command line. But that would
    // be quite messy to implement, so let's keep it simple for now.
    //
    if (optional<string> v = getenv ("BPKG_FETCH_CACHE"))
    {
      if (*v != "0" && *v != "false")
        parse (*v, "BPKG_FETCH_CACHE environment variable");
    }

    if (co.fetch_cache_specified ())
      parse (co.fetch_cache (), "--fetch-cache option");

    if (co.offline ())
      r.offline = true;

    return r;
  }

  optional<bool> fetch_cache::
  enabled (const common_options& co)
  {
    if (co.no_fetch_cache ())
      return false;
    else if (optional<string> v = getenv ("BPKG_FETCH_CACHE"))
    {
      if (*v == "0" || *v == "false")
        return false;
    }

    return nullopt;
  }

  fetch_cache::
  fetch_cache (const common_options& co, const database* db)
  {
    if (!ops_enabled_)
      ops_enabled_ = enabled (co);

    enabled_ =
      *ops_enabled_                         ? **ops_enabled_ :
      db != nullptr && db->fetch_cache_mode ? *db->fetch_cache_mode != "false" :
      true; // Enabled by default.

    // Initialize options mode. We have to do it regardless of whether the
    // cache is enabled due to offline().
    //
    if (!ops_mode_)
      ops_mode_ = mode (co);

    if (!enabled_)
      return;

    // Calculate effective mode for this configuration.
    //
    cache_mode m (*ops_mode_);

    if ((!m.src || !m.trust) && db != nullptr && db->fetch_cache_mode)
    {
      // This is effective mode, meaning it should only contain final values
      // without any overrides. Should be fast to parse every time without
      // caching (typically it will be just `src`).
      //
      const string& s (*db->fetch_cache_mode);

      for (size_t b (0), e (0), n; (n = next_word (s, b, e, ',')) != 0; )
      {
        if      (s.compare (b, n, "src") == 0      && !m.src)   m.src = true;
        else if (s.compare (b, n, "no-src") == 0   && !m.src)   m.src = false;
        else if (s.compare (b, n, "trust") == 0    && !m.trust) m.trust = true;
        else if (s.compare (b, n, "no-trust") == 0 && !m.trust) m.trust = false;
      }
    }

    // Defaults.
    //
    if (!m.src) m.src = false;
    if (!m.trust) m.trust = true;

    src_ = *m.src;
    trust_ = *m.trust;

    // Get specified or calculate default cache directories.
    //
    // Note that we need to calculate sp_directory even if shared src is
    // disabled since the database file may be there (see open() for details).
    //
    if (np_directory_.empty ())
    {
      const char* w (nullptr);
      try
      {
        if (co.fetch_cache_path_specified ())
        {
          w = "--fetch-cache-path option";

          np_directory_ = co.fetch_cache_path ();
        }
        else if (optional<string> v = getenv ("BPKG_FETCH_CACHE_PATH"))
        {
          w = "BPKG_FETCH_CACHE_PATH environment variable";

          np_directory_ = dir_path (move (*v));
        }

        if (np_directory_.empty ())
        {
          dir_path h;
          try
          {
            w = "user's home directory";
            h = path::home_directory ();
          }
          catch (const system_error&)
          {
            fail << "unable to obtain user's home directory to derive "
                 << "local fetch cache path" <<
              info << "use --fetch-cache-path option or BPKG_FETCH_CACHE_PATH "
                   << "environment variable to specify explicitly" <<
              info << "use --no-fetch-cache to disable caching";
          }

#ifndef _WIN32
          if (optional<string> v = getenv ("XDG_CACHE_HOME"))
          {
            w = "XDG_CACHE_HOME environment variable";
            np_directory_ = dir_path (move (*v));
          }
          else
          {
            w = "user's home directory";
            np_directory_ = h;
            np_directory_ /= ".cache";
          }

          np_directory_ /= "build2";
#else
          if (optional<string> v = getenv ("LOCALAPPDATA"))
          {
            w = "LOCALAPPDATA environment variable";
            np_directory_ = dir_path (move (*v));
          }
          else
          {
            w = "user's home directory";
            np_directory_ = h;
            np_directory_ /= "AppData";
            np_directory_ /= "Local";
          }

          np_directory_ /= "build2";
          np_directory_ /= "cache";
#endif

          w = "user's home directory";
          sp_directory_ = move (h);
          sp_directory_ /= ".build2";
          sp_directory_ /= "cache";
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
    if (session_.empty ())
    {
      if (co.fetch_cache_session_specified ())
        session_ = co.fetch_cache_session ();
      else if (optional<string> v = getenv ("BPKG_FETCH_CACHE_SESSION"))
        session_ = move (*v);

      if (session_.empty ())
        session_ = uuid::generate ().string ();
    }
  }

  static const path   db_file_name   ("fetch-cache.sqlite3");
  static const path   db_lock_name   ("fetch-cache.lock");
  static const string db_schema_name ("fetch-cache");

  // Register the data migration functions.
  //
#if 0
  template <odb::schema_version v>
  using migration_entry = odb::data_migration_entry<v, FETCH_CACHE_SCHEMA_VERSION_BASE>;

  static const migration_entry<2>
  migrate_v2 ([] (odb::database& db)
  {
  },
  db_schema_name);
#endif

  // Throw odb::timeout if the lock is busy.
  //
  void fetch_cache::
  lock ()
  {
    if (!exists (np_directory_))
      mk_p (np_directory_);

    path f (np_directory_ / db_lock_name);

    try
    {
      // Essentially the same code as in open() below.
      //
      unique_ptr<connection_factory> cf (new single_connection_factory);

      lock_.reset (
        new odb::sqlite::database (
          f.string (),
          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
          true,                  // Enable FKs.
          "",                    // Default VFS.
          move (cf)));

      connection_ptr c (lock_->connection ());
      c->execute ("PRAGMA locking_mode = EXCLUSIVE");
      transaction t (c->begin_exclusive ());
      t.commit ();
    }
    catch (const database_exception& e)
    {
      fail << f << ": " << e.message ();
    }
  }

  void fetch_cache::
  open (tracer& tr)
  {
    assert (enabled () && !is_open ());

    tracer trace ("fetch_cache::open");

    for (;;) // Lock wait loop.
    {
      path f; // Cache database path.
      try
      {
        // Find the cache database file, which can be in one of two
        // directories (sp or np; see above). See the file_cache class
        // documentation for details on this file's movements.
        //
        bool create (false);
        {
          // There are various race conditions if several instances of bpkg
          // try to do this at the same time. So we will use another SQLite
          // database as a file lock that is always stored in np_directory_.
          // Note that we can omit this lock if we found the cache database in
          // sp_directory_ since this is its final destination. Note also that
          // if we do grab the lock, then we must hold it until close() since
          // another instance could try to move the cache database from
          // underneath us.
          //
          // Naturally, we also don't need the lock if sp and np are the same
          // directory.
          //
          path sf;

          if (!sp_directory_.empty ())
          {
            f = sp_directory_ / db_file_name;
            if (!exists (f))
            {
              // Grab the file lock and retest.
              //
              lock ();

              if (!exists (f))
              {
                sf = move (f);
                f.clear ();
              }
            }
          }

          if (f.empty ())
          {
            auto cleanup = [] (const dir_path& d)
            {
              if (exists (d))
                rm_r (d);
            };

            f = np_directory_ / db_file_name;

            // True if the cache database should be in the sp directory.
            //
            bool sp (cache_src () && !sp_directory_.empty ());

            if (exists (f))
            {
              // Move it if it should be in sp.
              //
              if (sp)
              {
                // Clean up the sp_directory_ data subdirectories.
                //
                cleanup (dir_path (sp_directory_) /= "src");

                mk_p (sp_directory_);

                // We also have to move the rollback journal, if any. For
                // background, see: https://www.sqlite.org/tempfiles.html
                //
                // Note that we move it first to prevent the above check from
                // seeing the database without its journal.
                //
                path rf (f + "-journal");
                if (exists (rf))
                  mv (rf, sf + "-journal");

                mv (f, sf);

                f = move (sf);
              }
            }
            else
            {
              // Create.
              //
              if (sp)
              {
                f = move (sf);

                // Clean up the sp_directory_ data subdirectories.
                //
                cleanup (dir_path (sp_directory_) /= "src");
              }

              // Clean up the np_directory_ data subdirectories.
              //
              cleanup (dir_path (np_directory_) /= "pkg");
              cleanup (dir_path (np_directory_) /= "git");

              mk_p (sp ? sp_directory_ : np_directory_);

              create = true;
            }
          }
        }

        // Open/create the database. We don't need the thread pool.
        //
        unique_ptr<connection_factory> cf (new single_connection_factory);

        db_.reset (
          new odb::sqlite::database (
            f.string (),
            SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0),
            true,                  // Enable FKs.
            "",                    // Default VFS.
            move (cf)));

        auto& db (*db_);

        db.tracer (trace);

        // Lock the database for as long as the connection is active. First we
        // set locking_mode to EXCLUSIVE which instructs SQLite not to release
        // any locks until the connection is closed. Then we force SQLite to
        // acquire the write lock by starting exclusive transaction. See the
        // locking_mode pragma documentation for details. This will also fail
        // if the database is inaccessible (e.g., file does not exist, already
        // used by another process, etc).
        //
        {
          connection_ptr c (db.connection ());
          c->execute ("PRAGMA locking_mode = EXCLUSIVE");
          transaction t (c->begin_exclusive ());

          const string& sn (db_schema_name);

          if (create)
          {
            // Create the new schema.
            //
            if (db.schema_version (sn) != 0)
              fail << f << ": already has database schema";

            schema_catalog::create_schema (db, sn);
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

        db.tracer (tr); // Switch to the caller's tracer.
        break;
      }
      catch (odb::timeout&)
      {
        // Note that this handles both waiting on the lock database and the
        // actual cache database (see above for details). This is the reason
        // why we use np_directory_ in diagnostics: when trying to grab the
        // lock database, we don't yet know where the cache database should
        // be.
        //
        db_.reset ();
        lock_.reset ();

        // @@ FC: sleep and retry, also issue progress diagnostics.
        //
        // Maybe we should first wait up to 200ms before issuing progress?

        info << "fetch cache in " << np_directory_
             << " is already used by another process";
      }
      catch (const database_exception& e)
      {
        // Note: this error can only be about the cache database.
        //
        fail << f << ": " << e.message () << endf;
      }
    }
  }

  void fetch_cache::
  close ()
  {
    if (gc_thread_.joinable ())
      stop_gc (true /* ignore_errors */);

    // The tracer could already be destroyed (e.g., if called from the
    // destructor due to an exception-caused stack unwinding), so switch to
    // ours.
    //
    tracer trace ("fetch_cache::close");

    if (db_ != nullptr)
      db_->tracer (&trace);

    db_.reset ();
    lock_.reset ();
  }

  bool fetch_cache::
  enabled () const
  {
    return enabled_;
  }

  bool fetch_cache::
  offline () const
  {
    return ops_mode_->offline ? *ops_mode_->offline : false;
  }

  bool fetch_cache::
  offline (const common_options& co)
  {
    if (!ops_mode_)
      ops_mode_ = mode (co);

    return ops_mode_->offline ? *ops_mode_->offline : false;
  }

  bool fetch_cache::
  cache_src () const
  {
    return enabled_ && src_;
  }

  bool fetch_cache::
  cache_trust () const
  {
    return enabled_ && trust_;
  }

  void fetch_cache::
  garbage_collector ()
  {
    auto& db (*db_);

    // Switch to our own tracer.
    //
    tracer trace ("fetch_cache::garbage_collector");
    auto tg = make_guard ([o = db.tracer (), &db] () {db.tracer (o);});
    db.tracer (trace);

    timestamp three_months_ago (system_clock::now () - chrono::months (3));

    try
    {
      transaction t (db);

      auto stop = [this, &t] ()
      {
        if (gc_stop_.load (memory_order_consume))
        {
          t.commit ();
          return true;
        }
        else
          return false;
      };

      // @@ TODO: trust (1 year?)

      for (pkg_repository_metadata& o:
             db.query<pkg_repository_metadata> (
               /*query<pkg_repository_metadata>::access_time < three_months_ago*/))
      {
        if (stop ())
          return;

        // @@ TODO: delete filesystem entries, delete object.
        db.erase (o);

        if (stop ())
          return;
      }

      // @@ TODO: pkg archives (3 months).

      t.commit ();
    }
    catch (const database_exception& e)
    {
      gc_error_ << error << db.name () << ": " << e.message ();
    }
  }

  void fetch_cache::
  start_gc ()
  {
    assert (is_open () && !gc_thread_.joinable () && gc_error_.empty ());

    gc_stop_.store (false, memory_order_relaxed);
    gc_thread_ = thread (&fetch_cache::garbage_collector, this);
  }

  void fetch_cache::
  stop_gc (bool ie)
  {
    assert (is_open () && gc_thread_.joinable ());

    gc_stop_.store (true, memory_order_release);
    gc_thread_.join ();

    if (!ie && gc_error_.full ())
    {
      gc_error_.flush ();
      throw failed ();
    }
  }

  bool fetch_cache::
  load_pkg_repository_auth (const string& id)
  {
    assert (is_open () && !gc_thread_.joinable ());

    auto& db (*db_);

    try
    {
      transaction t (db);

      bool r (db.query_value<pkg_repository_auth_count> (
                query<pkg_repository_auth_count>::id == id) != 0);

      t.commit ();

      return r;
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message () << endf;
    }
  }

  void fetch_cache::
  save_pkg_repository_auth (string id, string fingerprint, string name)
  {
    assert (is_open () && !gc_thread_.joinable ());

    auto& db (*db_);

    try
    {
      transaction t (db);

      pkg_repository_auth a {move (id), move (fingerprint), move (name)};
      db.persist (a);

      t.commit ();
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }
  }

  static dir_path pkg_repository_metadata_directory_;

  optional<fetch_cache::loaded_pkg_repository_metadata> fetch_cache::
  load_pkg_repository_metadata (const repository_url& u)
  {
    assert (is_open () && !gc_thread_.joinable ());

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
        ((dir_path (np_directory_) /= "pkg") /= "metadata");
    }

    auto& db (*db_);

    try
    {
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
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }

    return r;
  }

  fetch_cache::saved_pkg_repository_metadata fetch_cache::
  save_pkg_repository_metadata (const repository_url& u,
                                string repositories_checksum,
                                string packages_checksum)
  {
    assert (is_open () && !gc_thread_.joinable () &&
            !pkg_repository_metadata_directory_.empty ());

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

    // Metadata file paths.
    //
    path rf;
    path pf;

    auto& db (*db_);

    try
    {
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
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }

    return saved_pkg_repository_metadata {move (rf), move (pf)};
  }

  static dir_path pkg_repository_package_directory_;

  optional<fetch_cache::loaded_pkg_repository_package> fetch_cache::
  load_pkg_repository_package (const package_id& id)
  {
    assert (is_open () && !gc_thread_.joinable ());

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
        ((dir_path (np_directory_) /= "pkg") /= "packages");
    }

    auto& db (*db_);

    try
    {
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

          r = loaded_pkg_repository_package {
            move (f), move (p.checksum), move (p.repository)};
        }
      }

      t.commit ();
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }

    return r;
  }

  path fetch_cache::
  save_pkg_repository_package (package_id id,
                               version v,
                               path file,
                               string checksum,
                               repository_url repository)
  {
    assert (is_open () && !gc_thread_.joinable () &&
            !pkg_repository_package_directory_.empty ());

    // The overall plan is as follows:
    //
    // 1. Create new database entry with current access time. Remove the
    //    archive file, if exists.
    //
    // 2. Return the path the archive file should be moved to.

    if (!exists (pkg_repository_package_directory_))
      mk_p (pkg_repository_package_directory_);

    path r (pkg_repository_package_directory_ / file);

    auto& db (*db_);

    try
    {
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
        move (file),
        move (checksum),
        move (repository)};

      db.persist (p);

      t.commit ();
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }

    return r;
  }
}
