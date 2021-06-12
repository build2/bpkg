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
    // Add the unnamed self-association of the target type.
    //
    shared_ptr<configuration> sc (
      make_shared<configuration> (optional<string> (), "target"));

    db.persist (sc);
    db.execute ("UPDATE selected_package_prerequisites SET configuration = '" +
                sc->uuid.string () + "'");
  });

  static inline path
  cfg_path (const dir_path& d, bool create)
  {
    path f (d / bpkg_dir / "bpkg.sqlite3");

    if (!create && !exists (f))
      fail << d << " does not look like a bpkg configuration directory";

    return f;
  }

  // The BPKG_OPEN_CONFIGS environment variable.
  //
  // Automatically set it to the configuration directory path and clear in the
  // main database constructor and destructor, respectively. Also append the
  // attached database configuration paths in their constructors and clear
  // them in detach_all(). The paths are absolute, normalized, double-quoted,
  // and separated with spaces.
  //
  static const string open_name ("BPKG_OPEN_CONFIGS");

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
        config (normalize (d, "configuration")),
        config_orig (d)
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
      using odb::schema_catalog;

      impl_->conn->execute ("PRAGMA locking_mode = EXCLUSIVE");

      add_env (true /* reset */);
      auto g (make_exception_guard ([] () {unsetenv (open_name);}));

      {
        sqlite::transaction t (impl_->conn->begin_exclusive ());

        if (create != nullptr)
        {
          // Create the new schema and persist the self-association.
          //
          if (schema_version () != 0)
            fail << sqlite::database::name () << ": already has database "
                 << "schema";

          schema_catalog::create_schema (*this);

          persist (*create); // Also assigns association id.

          // Cache the configuration information.
          //
          cache_config (create->uuid, create->name, create->type);
        }
        else
        {
          // Migrate the associated databases cluster.
          //
          migrate ();

          // Cache the configuration information.
          //
          shared_ptr<configuration> c (load<configuration> (0));
          cache_config (c->uuid, move (c->name), move (c->type));

          // Load the system repository, if requested.
          //
          if (sys_rep)
            load_system_repository ();
        }

        // Migrate the pre-associated database and the database cluster it
        // belongs to.
        //
        if (pre_assoc != nullptr)
          attach (*pre_assoc).migrate ();

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
    catch (const sqlite::database_exception& e)
    {
      fail << sqlite::database::name () << ": " << e.message ();
    }

    tracer (tr);

    // Note: will be leaked if anything further throws.
    //
    ig.release ();
  }

  // NOTE: if we ever load/persist any dynamically allocated objects in this
  // constructor, make sure such objects do not use the session or the session
  // is temporarily suspended in the attach() function (see its implementation
  // for the reasoning note) since the database will be moved.
  //
  database::
  database (impl* i,
            const dir_path& d,
            std::string schema,
            bool sys_rep)
      : sqlite::database (i->conn,
                          cfg_path (d, false /* create */).string (),
                          move (schema)),
        config (d),
        impl_ (i)
  {
    bpkg::tracer trace ("database");

    // Derive the configuration original directory path.
    //
    database& mdb (main_database ());

    if (mdb.config_orig.relative ())
    {
      // Fallback to absolute path if the configuration is on a different
      // drive on Windows.
      //
      if (optional<dir_path> c = config.try_relative (current_directory ()))
        config_orig = move (*c);
      else
        config_orig = config;
    }
    else
      config_orig = config;

    try
    {
      tracer_guard tg (*this, trace);

      // Cache the configuration information.
      //
      shared_ptr<configuration> c (load<configuration> (0));
      cache_config (c->uuid, move (c->name), move (c->type));

      // Load the system repository, if requested.
      //
      if (sys_rep)
        load_system_repository ();
    }
    catch (const sqlite::database_exception& e)
    {
      fail << sqlite::database::name () << ": " << e.message ();
    }

    add_env ();

    // Set the tracer used by the associated configurations cluster.
    //
    sqlite::database::tracer (mdb.tracer ());
  }

  database::
  ~database ()
  {
    if (impl_ != nullptr && // Not a moved-from database?
        main ())
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
        config_orig (move (db.config_orig)),
        system_repository (move (db.system_repository)),
        impl_ (db.impl_),
        explicit_associations_ (move (db.explicit_associations_)),
        implicit_associations_ (move (db.implicit_associations_))
  {
    db.impl_ = nullptr; // See ~database().
  }

  void database::
  add_env (bool reset) const
  {
    using std::string;

    string v;

    if (!reset)
    {
      if (optional<string> e = getenv (open_name))
        v = move (*e);
    }

    v += (v.empty () ? "\"" : " \"") + config.string () + '"';

    setenv (open_name, v);
  }

  void database::
  tracer (tracer_type* t)
  {
    main_database ().sqlite::database::tracer (t);

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
        fail << "configuration " << config_orig << " is too old";

      if (sv > scv)
        fail << "configuration " << config_orig << " is too new";

      // Note that we need to migrate the current database before the
      // associated ones to properly handle association cycles.
      //
      schema_catalog::migrate (*this);

      for (auto& c: query<configuration> (odb::query<configuration>::id != 0))
      {
        dir_path d (c.effective_path (config));

        // Remove the dangling implicit association.
        //
        if (!c.expl && !exists (d))
        {
          warn << "implicit association " << c.path << " of configuration "
               << config_orig << " does not exist, removing";

          erase (c);
          continue;
        }

        attach (d).migrate ();
      }
    }
  }

  void database::
  cache_config (const uuid_type& u, optional<std::string> n, std::string t)
  {
    uuid = u;
    name = move (n);
    type = move (t);
  }

  void database::
  load_system_repository ()
  {
    assert (!system_repository); // Must only be loaded once.

    system_repository = bpkg::system_repository ();

    // Query for all the packages with the system substate and enter their
    // versions into system_repository as non-authoritative. This way an
    // available_package (e.g., a stub) will automatically "see" system
    // version, if one is known.
    //
    assert (transaction::has_current ());

    for (const auto& p: query<selected_package> (
           odb::query<selected_package>::substate == "system"))
      system_repository->insert (p.name,
                                 p.version,
                                 false /* authoritative */);
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
      std::string schema;
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
      // are exclusive), start one to force database locking (see the above
      // locking_mode discussion for details).
      //
      sqlite::transaction t;
      if (!sqlite::transaction::has_current ())
        t.reset (begin_exclusive ());

      try
      {
        // NOTE: we need to be careful here not to bind any persistent objects
        // the database constructor may load/persist to the temporary database
        // object in the session cache.
        //
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
    assert (main ());

    explicit_associations_.clear ();
    implicit_associations_.clear ();

    for (auto i (impl_->attached_map.begin ());
         i != impl_->attached_map.end (); )
    {
      i->second.detach ();
      i = impl_->attached_map.erase (i);
    }

    // Remove the detached databases from the environment.
    //
    add_env (true /* reset */);
  }

  void database::
  verify_association (const configuration& ac, database& adb)
  {
    const dir_path& c (adb.config_orig);

    if (ac.uuid != adb.uuid)
      fail << "configuration " << c << " uuid mismatch" <<
        info << "uuid " << adb.uuid <<
        info << (!ac.expl ? "implicitly " : "") << "associated with "
             << config_orig << " as " << ac.uuid;

    if (ac.type != adb.type)
      fail << "configuration " << c << " type mismatch" <<
        info << "type " << adb.type <<
        info << (!ac.expl ? "implicitly " : "") << "associated with "
             << config_orig << " as " << ac.type;

    if (ac.effective_path (config) != adb.config)
      fail << "configuration " << c << " path mismatch" <<
        info << (!ac.expl ? "implicitly " : "") << "associated with "
           << config_orig << " as " << ac.path;
  }

  void database::
  attach_explicit (bool sys_rep)
  {
    assert (transaction::has_current ());

    if (explicit_associations_.empty ())
    {
      // Note that the self-association is implicit.
      //
      explicit_associations_.push_back (associated_config {0, name, *this});

      for (auto& ac: query<configuration> (odb::query<configuration>::expl))
      {
        database& db (attach (ac.effective_path (config), sys_rep));
        verify_association (ac, db);

        explicit_associations_.push_back (associated_config {*ac.id,
                                                             move (ac.name),
                                                             db});
        db.attach_explicit (sys_rep);
      }
    }
  }

  associated_databases& database::
  implicit_associations (bool ath, bool sys_rep)
  {
    assert (transaction::has_current ());

    // Note that cached implicit associations must at least contain the self-
    // association, if the databases are already attached and cached.
    //
    if (implicit_associations_.empty () && ath)
    {
      implicit_associations_.push_back (*this);

      using q = odb::query<configuration>;

      for (const auto& ac: query<configuration> (q::id != 0))
      {
        dir_path d (ac.effective_path (config));

        // Skip the dangling implicit association.
        //
        if (!ac.expl && !exists (d))
          continue;

        database& db (attach (d, sys_rep));

        // Verify the association integrity.
        //
        verify_association (ac, db);

        // If the association is explicit, also check if it is also implicit
        // (see cfg_add() for details) and skip if it is not.
        //
        if (ac.expl)
        {
          shared_ptr<configuration> cf (
            db.query_one<configuration> (q::uuid == uuid.string ()));

          if (cf == nullptr)
            fail << "configuration " << db.config_orig << " is associated "
                 << "with " << config_orig << " but latter is not "
                 << "implicitly associated with former";

          // While at it, verify the integrity of the other end of the
          // association.
          //
          db.verify_association (*cf, *this);

          if (!cf->expl)
            continue;
        }

        // If the explicitly associated databases are pre-attached, normally
        // to make the selected packages loadable, then we also pre-attach
        // explicit associations of the database being attached implicitly, by
        // the same reason. Indeed, think of loading the package dependent
        // from the implicitly associated database as a selected package.
        //
        if (!explicit_associations_.empty ())
          db.attach_explicit (sys_rep);

        implicit_associations_.push_back (db);
      }
    }

    return implicit_associations_;
  }

  associated_databases database::
  dependent_configs (bool sys_rep)
  {
    associated_databases r;

    // Add the associated database to the resulting list if it is of the
    // associating database type or this type is 'host'. Call itself
    // recursively for the implicitly associated configurations.
    //
    auto add = [&r, sys_rep] (database& db,
                              const std::string& t,
                              const auto& add)
    {
      if (!(db.type == t || t == "host") ||
          std::find (r.begin (), r.end (), db) != r.end ())
        return;

      r.push_back (db);

      const associated_databases& ads (
        db.implicit_associations (true /* attach */, sys_rep));

      // Skip the self-association.
      //
      for (auto i (ads.begin () + 1); i != ads.end (); ++i)
        add (*i, db.type, add);
    };

    add (*this, type, add);
    return r;
  }

  associated_databases database::
  dependency_configs (optional<bool> buildtime)
  {
    associated_databases r;

    bool allow_own_type  (!buildtime || !*buildtime);
    bool allow_host_type (!buildtime || *buildtime);

    // Add the associated database to the resulting list if it is of the
    // associating database type and allow_own_type is true or if it is of the
    // host type and allow_host_type is true. Call itself recursively for the
    // explicitly associated configurations.
    //
    // Note that the associated database of the associating database type is
    // not added if allow_own_type is false but its own associated databases
    // of the host type are added, if allow_host_type is true.
    //
    associated_databases descended; // Note: we may not add but still descend.
    auto add = [&r, allow_own_type, allow_host_type, &descended]
               (database& db, const std::string& t, const auto& add)
    {
      if (std::find (descended.begin (), descended.end (), db) !=
          descended.end ())
        return;

      descended.push_back (db);

      bool own  (db.type == t);
      bool host (db.type == "host");

      if (!own && !(allow_host_type && host))
        return;

      if ((allow_own_type && own) || (allow_host_type && host))
        r.push_back (db);

      const associated_configs& acs (db.explicit_associations ());

      // Skip the self-association.
      //
      for (auto i (acs.begin () + 1); i != acs.end (); ++i)
        add (i->db, db.type, add);
    };

    add (*this, type, add);
    return r;
  }

  database& database::
  find_attached (uint64_t id)
  {
    assert (!explicit_associations_.empty ());

    // Note that there shouldn't be too many databases, so the linear search
    // is OK.
    //
    auto r (find_if (explicit_associations_.begin (),
                     explicit_associations_.end (),
                     [&id] (const associated_config& ac)
                     {
                       return ac.id == id;
                     }));

    if (r == explicit_associations_.end ())
      fail << "no configuration with id " << id << " is associated with "
           << config_orig;

    return r->db;
  }

  database& database::
  find_attached (const std::string& name)
  {
    assert (!explicit_associations_.empty ());

    auto r (find_if (explicit_associations_.begin (),
                     explicit_associations_.end (),
                     [&name] (const associated_config& ac)
                     {
                       return ac.name && *ac.name == name;
                     }));

    if (r == explicit_associations_.end ())
      fail << "no configuration with name '" << name << "' is associated "
           << "with " << config_orig;

    return r->db;
  }

  database& database::
  find_dependency_config (const uuid_type& uid)
  {
    for (database& adb: dependency_configs ())
    {
      if (uid == adb.uuid)
        return adb;
    }

    fail << "no configuration with uuid " << uid << " is associated with "
         << config_orig << endf;
  }

  bool database::
  main ()
  {
    return *this == main_database ();
  }

  string database::
  string ()
  {
    return main () ? empty_string : '[' + config_orig.representation () + ']';
  }
}
