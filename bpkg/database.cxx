// file      : bpkg/database.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/database.hxx>

#include <sqlite3.h> // @@ TMP sqlite3_libversion_number()

#include <map>

#include <odb/schema-catalog.hxx>
#include <odb/sqlite/exceptions.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;

namespace bpkg
{
  namespace sqlite = odb::sqlite;

  // Configuration types.
  //
  const string host_config_type   ("host");
  const string build2_config_type ("build2");

  const string&
  buildtime_dependency_type (const package_name& nm)
  {
    return build2_module (nm) ? build2_config_type : host_config_type;
  }

  // Configuration names.
  //
  void
  validate_configuration_name (const string& s, const char* what)
  {
    if (s.empty ())
      fail << "empty " << what;

    if (!(alpha (s[0]) || s[0] == '_'))
      fail << "invalid " << what << " '" << s << "': illegal first character "
           << "(must be alphabetic or underscore)";

    for (auto i (s.cbegin () + 1), e (s.cend ()); i != e; ++i)
    {
      char c (*i);

      if (!(alnum (c) || c == '_' || c == '-'))
        fail << "invalid " << what << " '" << s << "': illegal character "
             << "(must be alphabetic, digit, underscore, or dash)";
    }
  }

  // Register the data migration functions.
  //
  // NOTE: remember to qualify table/index names with \"main\". if using
  // native statements.
  //
  template <odb::schema_version v>
  using migration_entry = odb::data_migration_entry<v, DB_SCHEMA_VERSION_BASE>;

  // @@ Since there is no proper support for dropping table columns not in
  //    SQLite prior to 3.35.5 nor in ODB, we will drop the
  //    available_package_dependency_alternatives.dep_* columns manually. We,
  //    however, cannot do it here since ODB will try to set the dropped
  //    column values to NULL at the end of migration. Thus, we will do it
  //    ad hoc after the below schema_catalog::migrate() call.
  //
  //    NOTE: remove the mentioned ad hoc migration when removing this
  //    function.
  //
  static const migration_entry<13>
  migrate_v13 ([] (odb::database& db)
  {
    // Note that
    // available_package_dependency_alternative_dependencies.alternative_index
    // is copied from available_package_dependency_alternatives.index and
    // available_package_dependency_alternative_dependencies.index is set to 0.
    //
    db.execute (
      "INSERT INTO \"main\".\"available_package_dependency_alternative_dependencies\" "
      "(\"name\", "
      "\"version_epoch\", "
      "\"version_canonical_upstream\", "
      "\"version_canonical_release\", "
      "\"version_revision\", "
      "\"version_iteration\", "
      "\"dependency_index\", "
      "\"alternative_index\", "
      "\"index\", "
      "\"dep_name\", "
      "\"dep_min_version_epoch\", "
      "\"dep_min_version_canonical_upstream\", "
      "\"dep_min_version_canonical_release\", "
      "\"dep_min_version_revision\", "
      "\"dep_min_version_iteration\", "
      "\"dep_min_version_upstream\", "
      "\"dep_min_version_release\", "
      "\"dep_max_version_epoch\", "
      "\"dep_max_version_canonical_upstream\", "
      "\"dep_max_version_canonical_release\", "
      "\"dep_max_version_revision\", "
      "\"dep_max_version_iteration\", "
      "\"dep_max_version_upstream\", "
      "\"dep_max_version_release\", "
      "\"dep_min_open\", "
      "\"dep_max_open\") "
      "SELECT "
      "\"name\", "
      "\"version_epoch\", "
      "\"version_canonical_upstream\", "
      "\"version_canonical_release\", "
      "\"version_revision\", "
      "\"version_iteration\", "
      "\"dependency_index\", "
      "\"index\", "
      "0, "
      "\"dep_name\", "
      "\"dep_min_version_epoch\", "
      "\"dep_min_version_canonical_upstream\", "
      "\"dep_min_version_canonical_release\", "
      "\"dep_min_version_revision\", "
      "\"dep_min_version_iteration\", "
      "\"dep_min_version_upstream\", "
      "\"dep_min_version_release\", "
      "\"dep_max_version_epoch\", "
      "\"dep_max_version_canonical_upstream\", "
      "\"dep_max_version_canonical_release\", "
      "\"dep_max_version_revision\", "
      "\"dep_max_version_iteration\", "
      "\"dep_max_version_upstream\", "
      "\"dep_max_version_release\", "
      "\"dep_min_open\", "
      "\"dep_max_open\" "
      "FROM \"main\".\"available_package_dependency_alternatives\"");
  });

  // @@ Since there is no proper support for dropping table columns not in
  //    SQLite prior to 3.35.5 nor in ODB, we will drop the
  //    available_package_dependencies.conditional column manually. We,
  //    however, cannot do it here since ODB will try to set the dropped
  //    column values to NULL at the end of migration. Thus, we will do it
  //    ad hoc after the below schema_catalog::migrate() call.
  //
  //    NOTE: remove the mentioned ad hoc migration when removing this
  //    function.
  //
  static const migration_entry<14>
  migrate_v14 ([] (odb::database&)
  {
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
            const dir_paths& pre_link,
            std::string str_repr)
      : sqlite::database (
          cfg_path (d, create != nullptr).string (),
          SQLITE_OPEN_READWRITE | (create != nullptr ? SQLITE_OPEN_CREATE : 0),
          true,                  // Enable FKs.
          "",                    // Default VFS.
          unique_ptr<sqlite::connection_factory> (
            new sqlite::serial_connection_factory)), // Single connection.
        config (normalize (d, "configuration")),
        config_orig (d),
        string (move (str_repr))
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
          // Create the new schema and persist the self-link.
          //
          if (schema_version () != 0)
            fail << sqlite::database::name () << ": already has database "
                 << "schema";

          schema_catalog::create_schema (*this);

          // To speed up the query_dependents() function create the multi-
          // column index for the configuration and prerequisite columns of
          // the selected_package_prerequisites table.
          //
          // @@ Use ODB pragma if/when support for container indexes is added.
          //
          execute (
            "CREATE INDEX "
            "selected_package_prerequisites_configuration_prerequisite_i "
            "ON selected_package_prerequisites (configuration, "
            "prerequisite)");

          persist (*create); // Also assigns link id.

          // Cache the configuration information.
          //
          cache_config (create->uuid, create->name, create->type);
        }
        else
        {
          // Migrate the linked databases cluster.
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

        // Migrate the pre-linked databases and the database clusters they
        // belong to.
        //
        for (const dir_path& d: pre_link)
          attach (d).migrate ();

        t.commit ();
      }

      // Detach potentially attached during migration the (pre-)linked
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

    string = '[' + config_orig.representation () + ']';

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

    // Set the tracer used by the linked configurations cluster.
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
        name (move (db.name)),
        type (move (db.type)),
        config (move (db.config)),
        config_orig (move (db.config_orig)),
        string (move (db.string)),
        system_repository (move (db.system_repository)),
        impl_ (db.impl_),
        explicit_links_ (move (db.explicit_links_)),
        implicit_links_ (move (db.implicit_links_))
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

      // Note that we need to migrate the current database before the linked
      // ones to properly handle link cycles.
      //
      schema_catalog::migrate (*this);

      // Note that the potential data corruption with `DROP COLUMN` is fixed
      // in 3.35.5.
      //
      // @@ TMP Get rid of manual column dropping when ODB starts supporting
      //    that properly. Not doing so will result in failure of the below
      //    queries.
      //
      if (sqlite3_libversion_number () >= 3035005)
      {
        auto drop = [this] (const char* table, const char* column)
        {
          execute (std::string ("ALTER TABLE \"main\".") + table +
                   " DROP COLUMN \"" + column + '"');
        };

        // @@ TMP See migrate_v13() for details.
        //
        if (sv < 13)
        {
          const char* cs[] = {"dep_name",
                              "dep_min_version_epoch",
                              "dep_min_version_canonical_upstream",
                              "dep_min_version_canonical_release",
                              "dep_min_version_revision",
                              "dep_min_version_iteration",
                              "dep_min_version_upstream",
                              "dep_min_version_release",
                              "dep_max_version_epoch",
                              "dep_max_version_canonical_upstream",
                              "dep_max_version_canonical_release",
                              "dep_max_version_revision",
                              "dep_max_version_iteration",
                              "dep_max_version_upstream",
                              "dep_max_version_release",
                              "dep_min_open",
                              "dep_max_open",
                              nullptr};

          for (const char** c (cs); *c != nullptr; ++c)
            drop ("available_package_dependency_alternatives", *c);
        }

        // @@ TMP See migrate_v14() for details.
        //
        if (sv < 14)
          drop ("available_package_dependencies", "conditional");
      }

      for (auto& c: query<configuration> (odb::query<configuration>::id != 0))
      {
        dir_path d (c.effective_path (config));

        // Remove the dangling implicit link.
        //
        if (!c.expl && !exists (d))
        {
          warn << "implicit link " << c.path << " of configuration "
               << config_orig << " no longer exists, removing";

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
    // NOTE: remember to update database(database&&) if changing anything
    // here.
    //
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
        sha256 h (d.string ());

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

    explicit_links_.clear ();
    implicit_links_.clear ();

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
  verify_link (const configuration& lc, database& ldb)
  {
    const dir_path& c (ldb.config_orig);

    if (lc.uuid != ldb.uuid)
      fail << "configuration " << c << " uuid mismatch" <<
        info << "uuid " << ldb.uuid <<
        info << (!lc.expl ? "implicitly " : "") << "linked with "
           << config_orig << " as " << lc.uuid;

    if (lc.type != ldb.type)
      fail << "configuration " << c << " type mismatch" <<
        info << "type " << ldb.type <<
        info << (!lc.expl ? "implicitly " : "") << "linked with "
           << config_orig << " as " << lc.type;

    if (lc.effective_path (config) != ldb.config)
      fail << "configuration " << c << " path mismatch" <<
        info << (!lc.expl ? "implicitly " : "") << "linked with "
           << config_orig << " as " << lc.path;
  }

  void database::
  attach_explicit (bool sys_rep)
  {
    assert (transaction::has_current ());

    if (explicit_links_.empty ())
    {
      // Note that the self-link is implicit.
      //
      explicit_links_.push_back (linked_config {0, name, *this});

      for (auto& lc: query<configuration> (odb::query<configuration>::expl))
      {
        database& db (attach (lc.effective_path (config), sys_rep));
        verify_link (lc, db);

        explicit_links_.push_back (linked_config {*lc.id, move (lc.name), db});
        db.attach_explicit (sys_rep);
      }
    }
  }

  linked_databases& database::
  implicit_links (bool attach_, bool sys_rep)
  {
    assert (transaction::has_current ());

    // Note that cached implicit links must at least contain the self-link,
    // if the databases are already attached and cached.
    //
    if (implicit_links_.empty () && attach_)
    {
      implicit_links_.push_back (*this);

      using q = odb::query<configuration>;

      for (const auto& lc: query<configuration> (q::id != 0))
      {
        dir_path d (lc.effective_path (config));

        // Skip the dangling implicit link.
        //
        if (!lc.expl && !exists (d))
        {
          if (verb > 1)
            info << "skipping dangling implicit backlink " << lc.path <<
              info << "use 'cfg-unlink --dangling' to clean up";

          continue;
        }

        database& db (attach (d, sys_rep));

        // Verify the link integrity.
        //
        verify_link (lc, db);

        // If the link is explicit, also check if it is also implicit (see
        // cfg_link() for details) and skip if it is not.
        //
        if (lc.expl)
        {
          shared_ptr<configuration> cf (backlink (db));

          if (!cf->expl)
            continue;
        }

        // If the explicitly linked databases are pre-attached, normally to
        // make the selected packages loadable, then we also pre-attach
        // explicit links of the database being attached implicitly, by the
        // same reason. Indeed, think of loading the package dependent from
        // the implicitly linked database as a selected package.
        //
        if (!explicit_links_.empty ())
          db.attach_explicit (sys_rep);

        implicit_links_.push_back (db);
      }
    }

    return implicit_links_;
  }

  shared_ptr<configuration> database::
  backlink (database& db)
  {
    using q = odb::query<configuration>;

    shared_ptr<configuration> cf (
      db.query_one<configuration> (q::uuid == uuid.string ()));

    if (cf == nullptr)
      fail << "configuration " << db.config_orig << " is linked with "
           << config_orig << " but latter is not implicitly linked "
           << "with former";

    // While at it, verify the integrity of the other end of the link.
    //
    db.verify_link (*cf, *this);
    return cf;
  }

  linked_databases database::
  dependent_configs (bool sys_rep)
  {
    linked_databases r;

    // Note that if this configuration is of a build-time dependency type
    // (host or build2) we need to be carefull during recursion and do not
    // cross the build-time dependency type boundary. So for example, for the
    // following implicit links only cfg1, cfg2, and cfg3 configurations are
    // included.
    //
    // cfg1 (this, host) -> cfg2 (host) -> cfg3 (build2) -> cfg4 (target)
    //
    // Add the linked database to the resulting list if it is of the linking
    // database type (t) or this type (t) is of the expected build-time
    // dependency type (bt).
    //
    auto add = [&r, sys_rep] (database& db,
                              const std::string& t,
                              const std::string& bt,
                              const auto& add)
    {
      if (!(db.type == t || t == bt) ||
          std::find (r.begin (), r.end (), db) != r.end ())
        return;

      r.push_back (db);

      const linked_databases& lds (db.implicit_links (true /* attach */,
                                                      sys_rep));

      // New boundary type.
      //
      const std::string& nbt (db.type == bt ? bt : empty_string);

      for (auto i (lds.begin_linked ()); i != lds.end (); ++i)
      {
        database& ldb (*i);
        add (ldb, db.type, nbt, add);

        // If this configuration is of the build2 type, then also add the
        // private host configurations of its implicitly linked
        // configurations.
        //
        if (db.type == build2_config_type)
        {
          if (database* hdb = ldb.private_config (host_config_type))
            add (*hdb, db.type, nbt, add);
        }
      }
    };

    add (*this,
         type,
         (type == host_config_type || type == build2_config_type
          ? type
          : empty_string),
         add);

    return r;
  }

  linked_databases database::
  dependency_configs (optional<bool> buildtime, const std::string& tp)
  {
    // The type only makes sense if build-time dependency configurations are
    // requested.
    //
    if (buildtime)
      assert (!*buildtime            ||
              tp == host_config_type ||
              tp == build2_config_type);
    else
      assert (tp.empty ());

    linked_databases r;

    // Allow dependency configurations of the dependent configuration own type
    // if all or runtime dependency configurations are requested.
    //
    bool allow_own_type  (!buildtime || !*buildtime);

    // Allow dependency configurations of the host type if all or regular
    // build-time dependency configurations are requested.
    //
    bool allow_host_type (!buildtime ||
                          (*buildtime && tp == host_config_type));

    // Allow dependency configurations of the build2 type if all or build2
    // module dependency configurations are requested.
    //
    bool allow_build2_type (!buildtime ||
                            (*buildtime && tp == build2_config_type));

    // Add the linked database to the resulting list if it is of the linking
    // database type and allow_own_type is true, or it is of the host type and
    // allow_host_type is true, or it is of the build2 type and
    // allow_build2_type is true. Call itself recursively for the explicitly
    // linked configurations.
    //
    // Note that the linked database of the linking database type is not added
    // if allow_own_type is false, however its own linked databases of the
    // host/build2 type are added, if allow_host_type/allow_build2_type is
    // true.
    //
    linked_databases chain; // Note: we may not add but still descend.
    auto add = [&r,
                allow_own_type,
                allow_host_type,
                allow_build2_type,
                &chain]
               (database& db,
                const std::string& t,
                const auto& add)
    {
      if (std::find (r.begin (),     r.end (),     db) != r.end () ||
          std::find (chain.begin (), chain.end (), db) != chain.end ())
        return;

      bool own    (db.type == t);
      bool host   (db.type == host_config_type);
      bool build2 (db.type == build2_config_type);

      // Bail out if we are not allowed to descend.
      //
      if (!own && !(allow_host_type && host) && !(allow_build2_type && build2))
        return;

      // Add the database to the list, if allowed, and descend afterwards.
      //
      if ((allow_own_type    && own)  ||
          (allow_host_type   && host) ||
          (allow_build2_type && build2))
        r.push_back (db);

      chain.push_back (db);

      {
        const linked_configs& lcs (db.explicit_links ());
        for (auto i (lcs.begin_linked ()); i != lcs.end (); ++i)
          add (i->db, db.type, add);
      }

      // If this is a private host configuration, then also add the parent's
      // explicitly linked configurations of the build2 type.
      //
      if (db.private_ () && db.type == host_config_type)
      {
        const linked_configs& lcs (db.parent_config ().explicit_links ());

        for (auto i (lcs.begin_linked ()); i != lcs.end (); ++i)
        {
          database& ldb (i->db);
          if (ldb.type == build2_config_type)
            add (ldb, db.type, add);
        }
      }

      chain.pop_back ();
    };

    add (*this, type, add);
    return r;
  }

  linked_databases database::
  dependency_configs (const package_name& n, bool buildtime)
  {
    return dependency_configs (buildtime,
                               (buildtime
                                ? buildtime_dependency_type (n)
                                : empty_string));
  }

  linked_databases database::
  dependency_configs ()
  {
    return dependency_configs (nullopt      /* buildtime */,
                               empty_string /* type */);
  }

  linked_databases database::
  cluster_configs (bool sys_rep)
  {
    linked_databases r;

    // If the database is not in the resulting list, then add it and its
    // dependent and dependency configurations, recursively.
    //
    auto add = [&r, sys_rep] (database& db, const auto& add)
    {
      if (std::find (r.begin (), r.end (), db) != r.end ())
        return;

      r.push_back (db);

      {
        linked_databases cs (db.dependency_configs ());
        for (auto i (cs.begin_linked ()); i != cs.end (); ++i)
          add (*i, add);
      }

      {
        linked_databases cs (db.dependent_configs (sys_rep));
        for (auto i (cs.begin_linked ()); i != cs.end (); ++i)
          add (*i, add);
      }
    };

    add (*this, add);

    return r;
  }

  database& database::
  find_attached (uint64_t id, bool s)
  {
    assert (!explicit_links_.empty ());

    // Note that there shouldn't be too many databases, so the linear search
    // is OK.
    //
    auto r (find_if (explicit_links_.begin (), explicit_links_.end (),
                     [&id] (const linked_config& lc)
                     {
                       return lc.id == id;
                     }));

    if (r == explicit_links_.end () || (!s && r == explicit_links_.begin ()))
      fail << "no configuration with id " << id << " is linked with "
           << config_orig;

    return r->db;
  }

  database& database::
  find_attached (const std::string& name, bool s)
  {
    assert (!explicit_links_.empty ());

    auto r (find_if (explicit_links_.begin (), explicit_links_.end (),
                     [&name] (const linked_config& lc)
                     {
                       return lc.name && *lc.name == name;
                     }));

    if (r == explicit_links_.end () || (!s && r == explicit_links_.begin ()))
      fail << "no configuration with name '" << name << "' is linked with "
           << config_orig;

    return r->db;
  }

  database& database::
  find_attached (const uuid_type& uid, bool s)
  {
    assert (!explicit_links_.empty ());

    auto r (find_if (explicit_links_.begin (), explicit_links_.end (),
                     [&uid] (const linked_config& lc)
                     {
                       return lc.db.get ().uuid == uid;
                     }));

    if (r == explicit_links_.end () || (!s && r == explicit_links_.begin ()))
      fail << "no configuration with uuid " << uid << " is linked with "
           << config_orig;

    return r->db;
  }

  database& database::
  find_attached (const dir_path& d, bool s)
  {
    assert (!explicit_links_.empty ());

    auto r (find_if (explicit_links_.begin (), explicit_links_.end (),
                     [&d] (const linked_config& lc)
                     {
                       return lc.db.get ().config == d;
                     }));

    if (r == explicit_links_.end () || (!s && r == explicit_links_.begin ()))
      fail << "no configuration with path " << d << " is linked with "
           << config_orig;

    return r->db;
  }

  database* database::
  try_find_dependency_config (const uuid_type& uid)
  {
    for (database& ldb: dependency_configs ())
    {
      if (uid == ldb.uuid)
        return &ldb;
    }

    return nullptr;
  }

  database& database::
  find_dependency_config (const uuid_type& uid)
  {
    if (database* db = try_find_dependency_config (uid))
      return *db;

    fail << "no configuration with uuid " << uid << " is linked with "
         << config_orig << endf;
  }

  database& database::
  parent_config (bool sys_rep)
  {
    assert (private_ ());

    dir_path pd (config.directory ().directory ()); // Parent configuration.
    const linked_databases& lds (implicit_links (true /* attach */, sys_rep));

    for (auto i (lds.begin_linked ()); i != lds.end (); ++i)
    {
      if (i->get ().config == pd)
        return *i;
    }

    // This should not happen normally and is likely to be the result of some
    // bpkg misuse.
    //
    fail << "configuration " << pd << " is not linked to its private "
         << "configuration " << config << endf;
  }

  database* database::
  private_config (const std::string& type)
  {
    assert (!explicit_links_.empty ());

    auto r (find_if (explicit_links_.begin_linked (), explicit_links_.end (),
                     [&type] (const linked_config& lc)
                     {
                       database& db (lc.db);
                       return db.private_ () && db.type == type;
                     }));

    return r != explicit_links_.end () ? &r->db.get () : nullptr;
  }

  bool database::
  main ()
  {
    return *this == main_database ();
  }

  // compare_lazy_ptr
  //
  bool compare_lazy_ptr::
  less (const odb::database& x, const odb::database& y) const
  {
    return static_cast<const database&> (x) < static_cast<const database&> (y);
  }
}
