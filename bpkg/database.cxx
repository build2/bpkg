// file      : bpkg/database.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/database.hxx>

#include <map>

#include <odb/schema-catalog.hxx>
#include <odb/sqlite/exceptions.hxx>

#include <libbutl/sha256.mxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;

namespace bpkg
{
  namespace sqlite = odb::sqlite;

  // Register the data migration functions.
  //
  // NOTE: remember to qualify table names with \"main\". if using native
  // statements.
  //
  template <odb::schema_version v>
  using migration_entry = odb::data_migration_entry<v, DB_SCHEMA_VERSION_BASE>;

  static const migration_entry<8>
  migrate_v8 ([] (odb::database& db)
  {
    for (shared_ptr<repository> r: pointer_result (db.query<repository> ()))
    {
      if (!r->name.empty ()) // Non-root repository?
      {
        r->local = r->location.local ();
        db.update (r);
      }
    }
  });

  static const migration_entry<9>
  migrate_v9 ([] (odb::database& db)
  {
    // Add the unnamed self configuration of the target type.
    //
    shared_ptr<configuration> sc (
      make_shared<configuration> (optional<string> (), "target"));

    db.persist (sc);
  });

  static inline path
  cfg_path (const dir_path& d, bool create)
  {
    path f (d / bpkg_dir / "bpkg.sqlite3");

    if (!create && !exists (f))
      fail << d << " does not look like a bpkg configuration directory";

    return f;
  }

  // Automatically set and clear the BPKG_OPEN_CONFIG environment variable in
  // the main database constructor and destructor.
  //
  static const string open_name ("BPKG_OPEN_CONFIG");

  struct database::impl
  {
    sqlite::connection_ptr conn; // Main connection.

    map<dir_path, database> attached_map;

    impl (sqlite::connection_ptr&& c): conn (move (c)) {}
  };

  database::
  database (const dir_path& d,
            configuration* create,
            odb::tracer& tr,
            bool pre_attach,
            bool sys_rep,
            const dir_path* pre_assoc)
      : sqlite::database (
          cfg_path (d, create != nullptr).string (),
          SQLITE_OPEN_READWRITE | (create != nullptr ? SQLITE_OPEN_CREATE : 0),
          true,                  // Enable FKs.
          "",                    // Default VFS.
          unique_ptr<sqlite::connection_factory> (
            new sqlite::serial_connection_factory)), // Single connection.
        config (normalize (d, "configuration"))
  {
    bpkg::tracer trace ("database");

    // Cache the (single) main connection we will be using.
    //
    unique_ptr<impl> ig ((impl_ = new impl (connection ())));

    try
    {
      tracer_guard tg (*this, trace);

      // Lock the database for as long as the connection is active. First we
      // set locking_mode to EXCLUSIVE which instructs SQLite not to release
      // any locks until the connection is closed. Then we force SQLite to
      // acquire the write lock by starting exclusive transaction. See the
      // locking_mode pragma documentation for details. This will also fail if
      // the database is inaccessible (e.g., file does not exist, already used
      // by another process, etc).
      //
      // Note that here we assume that any database that is ATTACHED within an
      // exclusive transaction gets the same treatment.
      //
      try
      {
        using odb::schema_catalog;

        impl_->conn->execute ("PRAGMA locking_mode = EXCLUSIVE");

        {
          sqlite::transaction t (impl_->conn->begin_exclusive ());

          if (create != nullptr)
          {
            // Create the new schema and persist the self configuration.
            //
            if (schema_version () != 0)
              fail << name () << ": already has database schema";

            schema_catalog::create_schema (*this);

            persist (*create); // Also assigns id.

            // Cache the configuration information.
            //
            cache_config (*create);
          }
          else
          {
            // Migrate the associated databases cluster.
            //
            migrate ();

            if (pre_assoc != nullptr)
              attach (*pre_assoc, false /* sys_rep */).migrate ();

            // Cache the configuration information.
            //
            shared_ptr<configuration> c (load<configuration> (0));
            cache_config (*c);

            if (sys_rep)
              load_system_repository ();
          }

          t.commit ();
        }

        // Detach potentially attached during migration the (pre-)associated
        // databases.
        //
        detach_all ();

        if (pre_attach)
        {
          sqlite::transaction t (begin_exclusive ());
          attach_explicit (sys_rep);
          t.commit ();
        }
      }
      catch (odb::timeout&)
      {
        fail << "configuration " << d << " is already used by another process";
      }
    }
    catch (const sqlite::database_exception& e)
    {
      fail << name () << ": " << e.message ();
    }

    tracer (tr);

    // @@ Don't we need to keep adding to BPKG_OPEN_CONFIG as we attach (and
    //    clear/reset in detach_all())?
    //
    setenv (open_name, normalize (d, "configuration").string ());

    ig.release (); // Will be leaked if anything further throws.
  }

  database::
  database (impl* i,
            const dir_path& d,
            string schema,
            bool sys_rep)
      : sqlite::database (i->conn,
                          cfg_path (d, false /* create */).string (),
                          move (schema)),
        config (d),
        impl_ (i)
  {
    bpkg::tracer trace ("database");

    try
    {
      tracer_guard tg (*this, trace);

      shared_ptr<configuration> cf (load<configuration> (0));
      cache_config (*cf);

      if (sys_rep)
        load_system_repository ();
    }
    catch (const sqlite::database_exception& e)
    {
      fail << name () << ": " << e.message ();
    }
  }

  database::
  ~database ()
  {
    if (impl_ != nullptr &&  // Not a moved-from database?
        schema ().empty ())  // Main database?
    {
      delete impl_;

      unsetenv (open_name);
    }
  }

  database::
  database (database&& db)
      : sqlite::database (move (db)),
        uuid (db.uuid),
        type (move (db.type)),
        config (move (db.config)),
        system_repository (move (db.system_repository)),
        impl_ (db.impl_),
        explicit_associations_ (move (db.explicit_associations_))
  {
    db.impl_ = nullptr; // See ~database().
  }

  void database::
  tracer (tracer_type* t)
  {
    sqlite::database::main_database ().tracer (t);

    for (auto& db: impl_->attached_map)
      db.second.sqlite::database::tracer (t);
  }

  void database::
  migrate ()
  {
    using odb::schema_catalog;

    odb::schema_version sv  (schema_version ());
    odb::schema_version scv (schema_catalog::current_version (*this));

    if (sv != scv)
    {
      if (sv < schema_catalog::base_version (*this))
        fail << "configuration '" << config << "' is too old";

      if (sv > scv)
        fail << "configuration '" << config << "' is too new";

      // Note that we need to migrate the current database before the
      // associated ones to properly handle association cycles.
      //
      schema_catalog::migrate (*this);

      for (auto& c: query<configuration> (odb::query<configuration>::id != 0))
        attach (c.resolve_path (config), false /* sys_rep */).migrate ();
    }
  }

  void database::
  cache_config (const configuration& cfg)
  {
    uuid = cfg.uuid;
    type = cfg.type;
  }

  void database::
  load_system_repository ()
  {
    // Query for all the packages with the system substate and enter their
    // versions into system_repository as non-authoritative. This way an
    // available_package (e.g., a stub) will automatically "see" system
    // version, if one is known.
    //
    assert (transaction::has_current ());

    for (const auto& p: query<selected_package> (
           odb::query<selected_package>::substate == "system"))
      system_repository.insert (p.name, p.version, false /* authoritative */);
  }

  database& database::
  attach (const dir_path& d, bool sys_rep)
  {
    assert (d.absolute () && d.normalized ());

    // Check if we are trying to attach the main database.
    //
    database& md (main_database ());
    if (d == md.config)
      return md;

    auto& am (impl_->attached_map);

    auto i (am.find (d));

    if (i == am.end ())
    {
      // We know from the implementation that 4-character schema names are
      // optimal. So try to come up with a unique abbreviated hash that is 4
      // or more characters long.
      //
      string schema;
      {
        butl::sha256 h (d.string ());

        for (size_t n (4);; ++n)
        {
          schema = h.abbreviated_string (n);

          if (find_if (am.begin (), am.end (),
                       [&schema] (const map<dir_path, database>::value_type& v)
                       {
                         return v.second.schema () == schema;
                       }) == am.end ())
            break;
        }
      }

      // If attaching out of an exclusive transaction (all our transactions
      // are exclusive), start one to force database locking (see the below
      // locking_mode discussion for details).
      //
      sqlite::transaction t;
      if (!sqlite::transaction::has_current ())
        t.reset (begin_exclusive ());

      try
      {
        i = am.insert (
          make_pair (d, database (impl_, d, move (schema), sys_rep))).first;
      }
      catch (odb::timeout&)
      {
        fail << "configuration " << d << " is already used by another process";
      }

      if (!t.finalized ())
        t.commit ();
    }

    return i->second;
  }

  void database::
  detach_all ()
  {
    assert (schema ().empty ());

    explicit_associations_ = nullopt;

    for (auto i (impl_->attached_map.begin ());
         i != impl_->attached_map.end (); )
    {
      i->second.detach ();
      i = impl_->attached_map.erase (i);
    }
  }

  void database::
  attach_explicit (bool sys_rep)
  {
    assert (transaction::has_current ());

    if (!explicit_associations_)
    {
      explicit_associations_ = associated_configs ();

      for (auto& ac: query<configuration> (odb::query<configuration>::expl))
      {
        database& db (attach (ac.resolve_path (config), sys_rep));
        explicit_associations_->push_back (associated_config {*ac.id, db});
        db.attach_explicit (sys_rep);
      }
    }
  }

  associated_configs& database::
  implicit_associations ()
  {
    assert (transaction::has_current ());

    // Note that cached implicit associations must at least contain the self
    // database.
    //
    if (implicit_associations_.empty ())
    {
      for (auto& ac: query<configuration> (!odb::query<configuration>::expl))
      {
        database& db (attach (ac.resolve_path (config), false /* sys_rep */));
        implicit_associations_.push_back (associated_config {*ac.id, db});
      }
    }

    return implicit_associations_;
  }

  database& database::
  find_attached (uint64_t i)
  {
    assert (explicit_associations_);

    // Check if this is the current database id.
    //
    if (i == 0)
      return *this;

    // Note that there shouldn't be too many databases, so the linear search
    // is OK.
    //
    auto r (find_if (explicit_associations_->begin (),
                     explicit_associations_->end (),
                     [&i] (const associated_config& ad)
                     {
                       return ad.id == i;
                     }));

    if (r != explicit_associations_->end ())
      return r->db;

    fail << "no configuration with id " << i << " is associated with "
         << config << endf;
  }

  database& database::
  find_attached (const uuid_type& i)
  {
    assert (explicit_associations_);

    // Check if this is the current database uuid.
    //
    if (i == uuid)
      return *this;

    auto r (find_if (explicit_associations_->begin (),
                     explicit_associations_->end (),
                     [&i] (const associated_config& ad)
                     {
                       return ad.db.get ().uuid == i;
                     }));

    if (r != explicit_associations_->end ())
      return r->db;

    fail << "no configuration with uuid " << i.string () << " is associated "
         << "with " << config << endf;
  }
}
