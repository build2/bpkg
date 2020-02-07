// file      : bpkg/database.cxx -*- C++ -*-
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
      setenv (open_name, normalize (d, "configuration").string ());
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

  static const migration_entry<6>
  migrate_v6 ([] (odb::database& db)
  {
    // Set the zero version revision to NULL.
    //
    auto migrate_rev = [&db] (const char* table, const char* column)
    {
      db.execute (string ("UPDATE ") + table + " SET " + column + " = NULL " +
                          "WHERE " + column + " = 0");
    };

    // The version package manifest value. Note: is not part of a primary key.
    //
    migrate_rev ("selected_package", "version_revision");

    // The depends package manifest value endpoint versions.
    //
    // Note that previously the zero and absent revisions had the same
    // semantics. Now the semantics differs and the zero revision is preserved
    // (see libbpkg/manifest.hxx for details).
    //
    migrate_rev ("selected_package_prerequisites", "min_version_revision");
    migrate_rev ("selected_package_prerequisites", "max_version_revision");

    migrate_rev ("available_package_dependency_alternatives",
                 "dep_min_version_revision");

    migrate_rev ("available_package_dependency_alternatives",
                 "dep_max_version_revision");
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
