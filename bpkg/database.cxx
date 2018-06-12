// file      : bpkg/database.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/database.hxx>

#include <odb/schema-catalog.hxx>
#include <odb/sqlite/exceptions.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/system-repository.hxx>

using namespace std;

namespace bpkg
{
  using namespace odb::sqlite;
  using odb::schema_catalog;

  // Use a custom connection factory to automatically set and clear the
  // BPKG_OPEN_CONFIG environment variable. A bit heavy-weight but seems like
  // the best option.
  //
  static const string open_name ("BPKG_OPEN_CONFIG");

  class conn_factory: public single_connection_factory // No need for pool.
  {
  public:
    conn_factory (const dir_path& d)
    {
      dir_path v (d);
      v.complete ();
      v.normalize ();

      setenv (open_name, v.string ());
    }

    virtual
    ~conn_factory ()
    {
      unsetenv (open_name);
    }
  };

  // Register the data migration functions.
  //
  template <odb::schema_version v>
  using migration_entry = odb::data_migration_entry<v, DB_SCHEMA_VERSION_BASE>;

  // Migrate tables that contain package version columns converting the
  // default zero version epoch to one, unless the version is a stub.
  //
  // Note that we can't really distinguish the default zero epoch from an
  // explicitly specified one, so will just update all of them, assuming that
  // it is currently unlikely that the epoch was specified explicitly for any
  // package version.
  //
  static const migration_entry<5>
  migrate_epoch_entry ([] (odb::database& db)
  {
    // Delay the foreign key constraint checks until we are done with all the
    // tables.
    //
    assert (transaction::has_current ());
    db.execute ("PRAGMA defer_foreign_keys = ON");

    auto update = [&db] (const string& table,
                         const string& version_prefix = "version")
    {
      string ec (version_prefix + "_epoch");

      db.execute ("UPDATE " + table + " SET " + ec + " = 1 " +
                  "WHERE " + ec + " = 0 AND NOT (" +
                  version_prefix + "_canonical_upstream = '' AND " +
                  version_prefix + "_canonical_release = '~')");
    };

    update ("available_package");
    update ("available_package_locations");
    update ("available_package_dependencies");
    update ("available_package_dependency_alternatives");
    update ("available_package_dependency_alternatives", "dep_min_version");
    update ("available_package_dependency_alternatives", "dep_max_version");
    update ("selected_package");
    update ("selected_package_prerequisites", "min_version");
    update ("selected_package_prerequisites", "max_version");

    db.execute ("PRAGMA defer_foreign_keys = OFF");
  });

  database
  open (const dir_path& d, tracer& tr, bool create)
  {
    tracer trace ("open");

    path f (d / bpkg_dir / "bpkg.sqlite3");

    if (!create && !exists (f))
      fail << d << " does not look like a bpkg configuration directory";

    try
    {
      database db (f.string (),
                   SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0),
                   true,                  // Enable FKs.
                   "",                    // Default VFS.
                   unique_ptr<connection_factory> (new conn_factory (d)));

      db.tracer (trace);

      // Lock the database for as long as the connection is active. First
      // we set locking_mode to EXCLUSIVE which instructs SQLite not to
      // release any locks until the connection is closed. Then we force
      // SQLite to acquire the write lock by starting exclusive transaction.
      // See the locking_mode pragma documentation for details. This will
      // also fail if the database is inaccessible (e.g., file does not
      // exist, already used by another process, etc).
      //
      using odb::sqlite::transaction; // Skip the wrapper.

      try
      {
        db.connection ()->execute ("PRAGMA locking_mode = EXCLUSIVE");
        transaction t (db.begin_exclusive ());

        if (create)
        {
          // Create the new schema.
          //
          if (db.schema_version () != 0)
            fail << f << ": already has database schema";

          schema_catalog::create_schema (db);
        }
        else
        {
          // Migrate the database if necessary.
          //
          schema_catalog::migrate (db);
        }

        t.commit ();
      }
      catch (odb::timeout&)
      {
        fail << "configuration " << d << " is already used by another process";
      }

      // Query for all the packages with the system substate and enter their
      // versions into system_repository as non-authoritative. This way an
      // available_package (e.g., a stub) will automatically "see" system
      // version, if one is known.
      //
      transaction t (db.begin ());

      for (const auto& p:
             db.query<selected_package> (
               query<selected_package>::substate == "system"))
        system_repository.insert (p.name, p.version, false);

      t.commit ();

      db.tracer (tr); // Switch to the caller's tracer.
      return db;
    }
    catch (const database_exception& e)
    {
      fail << f << ": " << e.message () << endf;
    }
  }
}
