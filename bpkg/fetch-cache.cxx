// file      : bpkg/fetch-cache.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch-cache.hxx>

#include <odb/query.hxx>
#include <odb/result.hxx>
#include <odb/schema-catalog.hxx>

#include <odb/sqlite/database.hxx>
#include <odb/sqlite/exceptions.hxx>

#include <libbutl/filesystem.hxx> // file_link_count()

#include <libbuild2/file.hxx> // is_src_root()

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

  // Transaction wrapper that allows starting a transaction and making it
  // current, for the duration of it's lifetime, in the presence of another
  // current transaction.
  //
  // Note that normally the cache functions will start the cache database
  // transactions when the caller has already started a configuration database
  // transaction.
  //
  class fetch_cache::transaction
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

  // Note that directory and session are only initialized if the cache is
  // enabled. The semi-precious directory is left empty if it's the same as
  // non-precious.
  //
  using cache_mode = fetch_cache::cache_mode;

  static optional<optional<bool>> ops_enabled_;
  static optional<cache_mode> ops_mode_;
  static string session_;

  // Non-precious.
  //
  static dir_path np_directory_;                      // ~/.cache/build2/
  static dir_path np_tmp_directory_;                  // ~/.cache/build2/tmp
  static dir_path pkg_repository_directory_;          // ~/.cache/build2/pkg
  static dir_path pkg_repository_metadata_directory_; // ~/.cache/build2/pkg/metadata
  static dir_path pkg_repository_package_directory_;  // ~/.cache/build2/pkg/packages
  static dir_path git_repository_state_directory_;    // ~/.cache/build2/git

  // Semi-precious.
  //
  // Note: the shared source directory is non-precious if sp_directory_ is
  // empty (--fetch-cache-path option is specified, etc).
  //
  // Note that we have a separate semi-precious tmp subdirectory in case np
  // and sp end up on different filesystems.
  //
  static dir_path sp_directory_;                      // ~/.build2/cache/
  static dir_path sp_tmp_directory_;                  // ~/.build2/cache/tmp
  static dir_path shared_source_directory_;           // ~/.build2/cache/src

  // If true, then print progress indicators while waiting for cache database
  // lock.
  //
  static bool progress_;

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
    mode (co, db);
  }

  fetch_cache::
  ~fetch_cache ()
  {
    close ();
  }

  void fetch_cache::
  mode (const common_options& co, const database* db)
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

        // While at it, calculate all the data directory paths.
        //
        pkg_repository_directory_ = (dir_path (np_directory_) /= "pkg");

        pkg_repository_metadata_directory_ =
          (dir_path (pkg_repository_directory_) /= "metadata");

        pkg_repository_package_directory_ =
          (dir_path (pkg_repository_directory_) /= "packages");

        git_repository_state_directory_ = (dir_path (np_directory_) /= "git");

        np_tmp_directory_ = (dir_path (np_directory_) /= "tmp");

        // If semi-precious directory is not used (--fetch-cache-path option
        // is specified, etc), then assume the shared source directory
        // non-precious and leave sp_tmp_directory_ empty.
        //
        if (!sp_directory_.empty ())
        {
          shared_source_directory_= (dir_path (sp_directory_) /= "src");
          sp_tmp_directory_ = (dir_path (sp_directory_) /= "tmp");
        }
        else
          shared_source_directory_= (dir_path (np_directory_) /= "src");
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

    // Progress indicators.
    //
    progress_ = (verb && !co.no_progress ()) || co.progress ();
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

    for (size_t i (0);; ++i) // Lock wait loop.
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
                cleanup (shared_source_directory_);

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
                cleanup (shared_source_directory_);
              }

              // Clean up the np_directory_ data subdirectories.
              //
              cleanup (pkg_repository_directory_);
              cleanup (git_repository_state_directory_);

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

        // Sleep 100 milliseconds and retry. Issue the first progress
        // indicator after 200 milliseconds and then every 5 seconds.
        //
        if (progress_ && (i == 2 || (i > 2 && (i - 2) % 50 == 0)))
          info << "fetch cache in " << np_directory_
               << " is used by another process, waiting";

        this_thread::sleep_for (chrono::milliseconds (100));
      }
      catch (const database_exception& e)
      {
        // Note: this error can only be about the cache database.
        //
        fail << f << ": " << e.message () << endf;
      }
    }

    // Clean up the temporary directories. Note: do it only once we have the
    // lock.
    //
    if (exists (np_tmp_directory_))
      rm_r (np_tmp_directory_, false /* dir_itself */);

    if (!sp_tmp_directory_.empty () && exists (sp_tmp_directory_))
      rm_r (sp_tmp_directory_, false /* dir_itself */);
  }

  void fetch_cache::
  close ()
  {
    // Note: may be open even if disabled (see mode()).

    if (active_gc ())
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

    auto since_epoch_ns = [] (timestamp t)
    {
      return chrono::duration_cast<chrono::nanoseconds> (
        t.time_since_epoch ()).count ();
    };

    timestamp now (system_clock::now ());

    uint64_t three_months_ago (since_epoch_ns (now - chrono::hours (24 * 90)));

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

      // Note: do the work in the most likely to be fruitful order.

      // Remove the package archives which have not been fetched in the last 3
      // months.
      //
      if (stop ()) return;
      for (pkg_repository_package& o:
             db.query<pkg_repository_package> (
               query<pkg_repository_package>::access_time < three_months_ago))
      {
        if (stop ()) return;

        path f (pkg_repository_package_directory_ / o.archive);

        if (verb >= 3)
          text << "rm " << f;

        try
        {
          try_rmfile (f);
        }
        catch (const system_error& e)
        {
          if (verb >= 3)
            warn << "unable to remove file " << f << ": " << e;

          continue;
        }

        db.erase (o);

        if (stop ()) return;
      }

      // Remove the unused shared source directories which have not been
      // unpacked or checked out in the last 3 months.
      //
      if (stop ()) return;
      for (shared_source_directory& o:
             db.query<shared_source_directory> (
               query<shared_source_directory>::access_time < three_months_ago))
      {
        // NOTE: recheck after every long operation (filesystem/database
        //       access).
        //
        if (stop ()) return;

        dir_path d (shared_source_directory_ / o.directory);

        // Skip the entry if the shared source directory is still used by some
        // package configurations.
        //

        // Skip the entry if the hard-links count for its src-root.build file
        // is greater than 1.
        //
        path p (d / o.src_root_file);

        try
        {
          if (file_link_count (p) > 1)
            continue;
        }
        catch (const system_error& e)
        {
          if (verb >= 3)
            warn << "unable to retrieve hard link count for " << p << ": " << e;

          continue;
        }

        // Remove non-existing configurations from the list of untracked
        // configurations (i.e., located on other filesystems). Skip the entry
        // if any configurations remain in the list. If the last configuration
        // has been removed, then update the access time and skip the entry to
        // give it another 3 months of lifetime for good measue (configuration
        // renamed, etc).
        //
        if (stop ()) return;
        db.load (o, o.untracked_configurations_section);
        paths& cs (o.untracked_configurations);

        size_t n (cs.size ());

        for (auto i (cs.begin ()); i != cs.end (); )
        {
          if (stop ()) return;

          const path& p (*i);

          try
          {
            // Note that the existing src-root.build file can be overwritten
            // by now and actually refer to some other source directory
            // (shared or not). Parsing it to make sure it still refers to
            // this shared source directory feels too hairy at the
            // moment. Let's keep it simple for now and assume that if it
            // exists, then it still refers to this source directory. The only
            // drawback is that we may keep a source directory in the cache
            // longer than necessary.
            //
            if (!file_exists (p))
              i = cs.erase (i);
            else
              ++i;
          }
          catch (const system_error& e)
          {
            if (verb >= 3)
              warn << "unable to stat path " << p << ": " << e;

            ++i;
          }
        }

        bool force_skip (false);

        if (cs.size () != n)
        {
          if (cs.empty ())
          {
            o.access_time = system_clock::now ();
            force_skip = true;
          }

          db.update (o); // Note: mutually exclusive with erase() below.
        }

        if (!cs.empty () || force_skip)
          continue;

        if (stop ()) return;

        // Remove the shared source directory and the database entry.
        //
        if (verb >= 3)
          text << "rm -r " << d;

        try
        {
          if (dir_exists (d))
            rmdir_r (d, true /* dir */);
        }
        catch (const system_error& e)
        {
          if (verb >= 3)
            warn << "unable to remove directory " << d << ": " << e;

          continue;
        }

        db.erase (o);

        if (stop ()) return;
      }

      // Remove the metadata for pkg repositories which have not been fetched
      // in the last 3 months.
      //
      if (stop ()) return;
      for (pkg_repository_metadata& o:
             db.query<pkg_repository_metadata> (
               query<pkg_repository_metadata>::access_time < three_months_ago))
      {
        if (stop ()) return;

        dir_path d (pkg_repository_metadata_directory_ / o.directory);

        if (verb >= 3)
          text << "rm -r " << d;

        try
        {
          if (dir_exists (d))
            rmdir_r (d, true /* dir */);
        }
        catch (const system_error& e)
        {
          if (verb >= 3)
            warn << "unable to remove directory " << d << ": " << e;

          continue;
        }

        db.erase (o);

        if (stop ()) return;
      }

      // Remove the git repositories which have not been fetched or checked
      // out in the last 3 months.
      //
      if (stop ()) return;
      for (git_repository_state& o:
             db.query<git_repository_state> (
               query<git_repository_state>::access_time < three_months_ago))
      {
        if (stop ()) return;

        dir_path d (git_repository_state_directory_ / o.directory);

        if (verb >= 3)
          text << "rm -r " << d;

        try
        {
          if (dir_exists (d))
            rmdir_r (d, true /* dir */);
        }
        catch (const system_error& e)
        {
          if (verb >= 3)
            warn << "unable to remove directory " << d << ": " << e;

          continue;
        }

        db.erase (o);

        if (stop ()) return;
      }

      // Note that the certificate validity is re-checked regardless if it is
      // trusted or not (see auth_cert() and auth_real()). Normally, a
      // certificate is replaced in the repository manifest before it is
      // expired, eventually is trusted by the user, and ends up in the cache
      // under the new id.
      //
      if (stop ()) return;
      for (pkg_repository_auth& o:
             db.query<pkg_repository_auth> (
               query<pkg_repository_auth>::end_date.is_not_null () &&
               query<pkg_repository_auth>::end_date < since_epoch_ns (now)))
      {
        if (stop ()) return;

        db.erase (o);

        if (stop ()) return;
      }

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
    // Note: should never be started when offline.
    //
    assert (is_open () && !offline () && !active_gc () && gc_error_.empty ());

    gc_stop_.store (false, memory_order_relaxed);
    gc_thread_ = thread (&fetch_cache::garbage_collector, this);
  }

  void fetch_cache::
  stop_gc (bool ie)
  {
    assert (is_open () && active_gc ());

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
    assert (is_open () && !active_gc ());

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
  save_pkg_repository_auth (string id,
                            string fingerprint,
                            string name,
                            optional<timestamp> end_date)
  {
    assert (is_open () && !active_gc ());

    auto& db (*db_);

    try
    {
      transaction t (db);

      pkg_repository_auth a {
        move (id), move (fingerprint), move (name), end_date};

      db.persist (a);

      t.commit ();
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }
  }

  // Convert the local repository URL path to lower case on Windows. Noop on
  // POSIX.
  //
  inline static repository_url
  canonicalize_url (repository_url&& u)
  {
    assert (u.path);

#ifdef _WIN32
    if (u.scheme == repository_protocol::file)
      *u.path = path (lcase (move (*u.path).string ()));
#endif

    return move (u);
  }

  optional<fetch_cache::loaded_pkg_repository_metadata> fetch_cache::
  load_pkg_repository_metadata (repository_url u)
  {
    assert (is_open () && !active_gc ());

    u = canonicalize_url (move (u));

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
          // Remove the database entry last, to make sure we are still
          // tracking the directory if its removal fails for any reason.
          //
          if (exists (d))
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
  save_pkg_repository_metadata (repository_url u,
                                string repositories_checksum,
                                string packages_checksum)
  {
    assert (is_open () && !active_gc ());

    u = canonicalize_url (move (u));

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
          move (u),
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

  optional<fetch_cache::loaded_pkg_repository_package> fetch_cache::
  load_pkg_repository_package (const package_id& id)
  {
    assert (is_open () && !active_gc ());

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
                               const path& archive,
                               bool mv,
                               string checksum,
                               repository_url repository)
  {
    assert (is_open () && !active_gc ());

    // The overall plan is as follows:
    //
    // 1. Create new database entry with current access time. Remove the
    //    archive file, if exists.
    //
    // 2. Move or hard-link/copy the archive to its permanent location.
    //
    // 3. Return the permanent archive path.

    path an (archive.leaf ());
    path r (pkg_repository_package_directory_ / an);

    // If the archive file already exists, probably as a result of some
    // previous failure, then remove it. Create the database entry last, to
    // make sure we are not referring to an invalid file if its removal fails
    // for any reason.
    //
    if (exists (r))
      rm (r);
    else if (!exists (pkg_repository_package_directory_))
      mk_p (pkg_repository_package_directory_);

    auto& db (*db_);

    try
    {
      transaction t (db);

      pkg_repository_package p {
        move (id),
        move (v),
        system_clock::now (),
        move (an),
        move (checksum),
        move (repository)};

      db.persist (p);

      t.commit ();
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }

    if (mv)
    {
      using bpkg::mv;

      // Note that the move operation can fallback to copy, if the source and
      // destination paths belong to different filesystems. Thus, to implement
      // the "write to temporary and atomically move into place" technique, we
      // move the archive in two steps: first, to the destination filesystem
      // under the temporary name and then rename it to the final name.
      //
      auto_rmfile rm (r + ".tmp");
      const path& p (rm.path);
      mv (archive, p);
      mv (p, r);
      rm.cancel ();
    }
    else
      hardlink (archive, r);

    return r;
  }

  static dir_path repository_dir ("repository");
  static dir_path ls_remote_file ("ls-remote.txt");

  // Canonicalize the repository URL by converting the path to lower case, if
  // the URL is local and we are running on Windows, and stripping the .git
  // extension, if present. The same logic as elsewhere (libbpkg, etc).
  //
  static repository_url
  canonicalize_git_url (repository_url&& u)
  {
    u = canonicalize_url (move (u));

    assert (u.path);

    path& up (*u.path);
    const char* e (up.extension_cstring ());

    if (e != nullptr && strcmp (e, "git") == 0)
      up.make_base ();

    return move (u);
  }

  inline static dir_path
  git_repository_state_name (const repository_url& u)
  {
    return dir_path (sha256 (u.string ()).abbreviated_string (16));
  }

  fetch_cache::loaded_git_repository_state fetch_cache::
  load_git_repository_state (repository_url u)
  {
    assert (is_open () && !active_gc () && !u.fragment);

    u = canonicalize_git_url (move (u));

    // The overall plan is as follows:
    //
    // 1. See if there is an entry for this URL in the database. If not, the
    //    state is absent.
    //
    // 2. Otherwise, check if the repository subdirectory exists in the
    //    repository state directory. If not, remove the state directory on
    //    disk, remove the entry from the database, and assume the state is
    //    absent.
    //
    // 3. Otherwise, the state is up-to-date if ls-remote.txt exists in the
    //    repository state directory and the current session matches the entry
    //    session or we are in the offline mode.
    //
    // 4. Otherwise, remove the ls-remote.txt file, if exists. The state is
    //    outdated.
    //
    // 5. For the absent state, create an empty repository state directory in
    //    the cache temporary directory. For other states, update the entry
    //    session and access time and move the repository state directory into
    //    the cache temporary directory.
    //
    // 6. Return the deduced state and paths to the repository directory and
    //    ls-remote.txt file in the cache temporary directory, regardless of
    //    whether they exist or not.

    loaded_git_repository_state r;

    auto& db (*db_);

    dir_path sd; // State directory for this repository.
    dir_path td; // Temporary directory for this repository.

    try
    {
      transaction t (db);

      git_repository_state s;
      if (db.find<git_repository_state> (u, s))
      {
        sd = git_repository_state_directory_ / s.directory;
        td = np_tmp_directory_ / s.directory;

        dir_path rd (sd / repository_dir);

        if (!exists (rd))
        {
          // Remove the database entry last, to make sure we are still
          // tracking the directory if its removal fails for any reason.
          //
          if (exists (sd))
            rm_r (sd);

          db.erase (s);

          r.state = loaded_git_repository_state::absent;
        }
        else
        {
          path lf (sd / ls_remote_file);

          // True if ls-remote exists and is up-to-date.
          //
          bool utd (exists (lf));

          if (utd)
          {
            utd = (offline () || s.session == session_);

            if (!utd)
              rm (lf);
          }

          s.session = session_;
          s.access_time = system_clock::now ();

          db.update (s);

          r.state = utd
            ? loaded_git_repository_state::up_to_date
            : loaded_git_repository_state::outdated;
        }
      }
      else
      {
        dir_path d (git_repository_state_name (u));

        sd = git_repository_state_directory_ / d;
        td = np_tmp_directory_ / d;

        if (exists (sd))
          rm_r (sd);

        r.state = loaded_git_repository_state::absent;
      }

      t.commit ();
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }

    if (exists (td))
      rm_r (td);
    else if (!exists (np_tmp_directory_))
      mk_p (np_tmp_directory_);

    if (r.state == loaded_git_repository_state::absent)
      mk (td);
    else
      mv (sd, td);

    r.repository = td / repository_dir;
    r.ls_remote = td / ls_remote_file;

    return r;
  }

  void fetch_cache::
  save_git_repository_state (repository_url u)
  {
    assert (is_open () && !active_gc () && !u.fragment);

    u = canonicalize_git_url (move (u));

    // The overall plan is as follows:
    //
    // 1. Try to load the current entry from the database. If absent, create
    //    new database entry with current session and access time.
    //
    // 2. Move the temporary repository state directory to its permanent
    //    location.

    auto& db (*db_);

    dir_path sd; // State directory for this repository.
    dir_path td; // Temporary directory for this repository.

    try
    {
      transaction t (db);

      git_repository_state s;
      if (db.find<git_repository_state> (u, s))
      {
        sd = git_repository_state_directory_ / s.directory;
        td = np_tmp_directory_ / s.directory;

        // If the repository directory already exists, probably as a result of
        // some previous failure, then remove it. Note that on the removal
        // failure we may end up referring to a broken repository. Given such
        // a situation is not very common, let's not complicate things here
        // and rely on the user to manually fix that on the recurring errors.
        //
        if (exists (sd))
          rm_r (sd);
      }
      else
      {
        dir_path d (git_repository_state_name (u));

        sd = git_repository_state_directory_ / d;
        td = np_tmp_directory_ / d;

        // If the repository directory already exists, then remove it. Create
        // the database entry last, to make sure we are not referring to a
        // broken repository if its removal fails for any reason.
        //
        if (exists (sd))
          rm_r (sd);

        git_repository_state rs {
          move (u), move (d), session_, system_clock::now ()};

        db.persist (rs);
      }

      t.commit ();
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }

    if (!exists (git_repository_state_directory_))
      mk_p (git_repository_state_directory_);

    mv (td, sd);
  }

  // Note that this function is not static to make sure that the global
  // variable git_repository_state_directory_ is already set (note: set by
  // mode(common_options, database*) is the fetch caching is enabled).
  //
  dir_path fetch_cache::
  git_repository_state_dir (repository_url u) const
  {
    assert (enabled ());

    u = canonicalize_git_url (move (u));
    return git_repository_state_directory_ / git_repository_state_name (u);
  }

  fetch_cache::loaded_shared_source_directory_state fetch_cache::
  load_shared_source_directory (const package_id& id, const version& v)
  {
    assert (is_open () && !active_gc ());

    // The overall plan is as follows:
    //
    // 1. See if there is an entry for this package id in the database. If
    //    not, return the temporary directory path.
    //
    // 2. Check if the source directory exists for this cache entry. If not,
    //    remove the entry from the database and return the temporary
    //    directory path.
    //
    // 3. Update entry access_time.
    //
    // 4. Return the permanent source directory path.

    loaded_shared_source_directory_state r;

    const dir_path& tmp_dir (!sp_tmp_directory_.empty ()
                             ? sp_tmp_directory_
                             : np_tmp_directory_);

    auto& db (*db_);

    try
    {
      transaction t (db);

      shared_source_directory sd;
      if (db.find<shared_source_directory> (id, sd))
      {
        dir_path d (shared_source_directory_ / sd.directory);

        if (!exists (d))
        {
          db.erase (sd);

          r = loaded_shared_source_directory_state {
            false /* present */, tmp_dir / sd.directory};
        }
        else
        {
          sd.access_time = system_clock::now ();

          db.update (sd);

          r = loaded_shared_source_directory_state {
            true /* present */, move (d)};
        }
      }
      else
      {
        r = loaded_shared_source_directory_state {
          false /* present */,
          tmp_dir / dir_path (id.name.string () + '-' + v.string ())};
      }

      t.commit ();
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }

    if (!r.present)
    {
      if (exists (r.directory))
        rm_r (r.directory);
      else if (!exists (tmp_dir))
        mk_p (tmp_dir);
    }

    return r;
  }

  dir_path fetch_cache::
  save_shared_source_directory (package_id id,
                                version v,
                                dir_path tmp_directory,
                                repository_url repository,
                                string origin_id)
  {
    assert (is_open () && !active_gc ());

    // The overall plan is as follows:
    //
    // 1. Create new database entry with current access time. Remove the
    //    source directory, if exists.
    //
    // 2. Move the temporary directory to its permanent location.
    //
    // 3. Return the permanent source directory path.

    dir_path n (tmp_directory.leaf ());
    assert (n.string () == id.name.string () + '-' + v.string ());

    dir_path r (shared_source_directory_ / n);

    // If the shared source directory already exists, probably as a result of
    // some previous failure, then remove it. Create the database entry last,
    // to make sure we are not referring to a broken directory if its removal
    // fails for any reason.
    //
    if (exists (r))
      rm (r);
    else if (!exists (shared_source_directory_))
      mk_p (shared_source_directory_);

    optional<bool> alt_naming;

    try
    {
      if (!build2::is_src_root (tmp_directory, alt_naming))
        fail << tmp_directory << " is not a package source directory";
    }
    catch (const build2::failed&)
    {
      throw failed (); // Assume the diagnostics has already been issued.
    }

    auto& db (*db_);

    try
    {
      transaction t (db);

      assert (alt_naming); // Wouldn't be here otherwise.

      shared_source_directory d {
        move (id),
        move (v),
        system_clock::now (),
        move (n),
        move (repository),
        move (origin_id),
        (*alt_naming ? alt_src_root_file : std_src_root_file)};

      db.persist (d);

      t.commit ();
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }

    mv (tmp_directory, r);

    return r;
  }

  static uint64_t
  hardlink_count (const path& p)
  {
    try
    {
      return file_link_count (p);
    }
    catch (const system_error& e)
    {
      fail << "unable to retrieve hard link count for " << p << ": " << e << endf;
    }
  }

  optional<fetch_cache::shared_source_directory_tracking> fetch_cache::
  load_shared_source_directory_tracking (const package_id& id)
  {
    assert (is_open () && !active_gc ());

    optional<shared_source_directory_tracking> r;

    auto& db (*db_);

    try
    {
      transaction t (db);

      shared_source_directory sd;
      if (db.find<shared_source_directory> (id, sd))
      {
        dir_path d (shared_source_directory_ / sd.directory);

        // Note that this function is not necessarily called right after
        // load_shared_source_directory() (think of package
        // re-configurations). Thus, let's check for the shared source
        // directory existence here as well.
        //
        if (exists (d))
        {
          path f (d / sd.src_root_file);
          size_t hc (hardlink_count (f));

          // This is tricky: to allow moving the cache around, we remove the
          // (potentially old) src-root.build file if nobody else is using it.
          // This way it will be recreated by the caller with the correct
          // path.
          //
          if (hc == 1)
          {
            rm (f);
            hc = 0;
          }

          r = shared_source_directory_tracking {move (d), hc};
        }
        else
          db.erase (sd);
      }

      t.commit ();
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }

    return r;
  }

  void fetch_cache::
  save_shared_source_directory_tracking (const package_id& id,
                                         const dir_path& conf,
                                         uint64_t use_count)
  {
    assert (is_open () && !active_gc ());
    assert (conf.absolute () && conf.normalized ());

    auto& db (*db_);

    try
    {
      transaction t (db);

      // Note that the cache shouldn't have been unlocked and so this object
      // should be there.
      //
      shared_source_directory sd;
      db.load<shared_source_directory> (id, sd);

      size_t hc (
        hardlink_count (
          shared_source_directory_ / sd.directory / sd.src_root_file));

      // If the hard link count hasn't changed after creation of the new
      // configuration, then assume that this configuration cannot be tracked
      // with the hard link count (e.g., located on a different filesystem)
      // and so add it to the list of untracked ones.
      //
      if (hc == use_count)
      {
        // Absolute and normalized by construction. Note that in the output
        // directories we always use standard naming.
        //
        path p (conf / std_src_root_file);

        db.load (sd, sd.untracked_configurations_section);
        paths& cs (sd.untracked_configurations);

        if (find (cs.begin (), cs.end (), p) == cs.end ())
          cs.push_back (move (p));
      }

      sd.access_time = system_clock::now ();

      db.update (sd);

      t.commit ();
    }
    catch (const database_exception& e)
    {
      fail << db.name () << ": " << e.message ();
    }
  }
}
