// file      : bpkg/database.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_DATABASE_HXX
#define BPKG_DATABASE_HXX

#include <type_traits> // remove_reference

#include <odb/query.hxx>
#include <odb/result.hxx>
#include <odb/session.hxx>

#include <odb/sqlite/database.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/diagnostics.hxx>
#include <bpkg/system-repository.hxx>

namespace bpkg
{
  using odb::query;
  using odb::result;
  using odb::session;

  class configuration;
  class database;

  struct linked_config
  {
    uint64_t                    id;
    optional<string>            name;
    reference_wrapper<database> db;   // Needs to be move-assignable.
  };

  // Used for the immediate explicit links which are normally not many (one
  // entry for the self-link).
  //
  using linked_configs = small_vector<linked_config, 2>;

  // In particular, is used for implicit links which can potentially be many.
  // Think of a dependency in a shared configuration with dependents in
  // multiple implicitly linked configurations.
  //
  using linked_databases = small_vector<reference_wrapper<database>, 16>;

  // Derive a custom database class that handles attaching/detaching
  // additional configurations.
  //
  class database: public odb::sqlite::database
  {
  public:
    using uuid_type = bpkg::uuid;

    // Create new main database.
    //
    // The specified self-link object is persisted and its uuid and type are
    // cached in the database object.
    //
    // If the pre-link list is not empty, then these configurations are
    // treated as linked configurations for schema migration purposes. If
    // specified, these paths should be absolute and normalized.
    //
    database (const dir_path& cfg,
              const shared_ptr<configuration>& self,
              odb::tracer& tr,
              const dir_paths& pre_link = dir_paths ())
        : database (cfg, self.get (), tr, false, false, pre_link)
    {
      assert (self != nullptr);
    }

    // Open existing main database.
    //
    // If configured non-system selected packages can potentially be loaded
    // from this database, then pass true as the pre_attach argument to
    // recursively pre-attach the explicitly linked configuration databases,
    // so that package prerequisites can be loaded from the linked
    // configurations as well (see _selected_package_ref::to_ptr()
    // implementation for details). Note that selected packages are loaded by
    // some functions internally (package_iteration(), etc). Such functions
    // are marked with the 'Note: loads selected packages.' note.
    //
    database (const dir_path& cfg,
              odb::tracer& tr,
              bool pre_attach,
              bool sys_rep = false,
              const dir_paths& pre_link = dir_paths ())
        : database (cfg, nullptr, tr, pre_attach, sys_rep, pre_link) {}

    ~database ();

    // Move-constructible but not move-assignable.
    //
    database (database&&);
    database& operator= (database&&) = delete;

    database (const database&) = delete;
    database& operator= (const database&) = delete;

    // Attach another (existing) database. The configuration directory should
    // be absolute and normalized.
    //
    // Note that if the database is already attached, then the existing
    // instance reference is returned and the sys_rep argument is ignored.
    //
    database&
    attach (const dir_path&, bool sys_rep = false);

    // Attach databases of all the explicitly linked configurations,
    // recursively. Must be called inside the transaction.
    //
    void
    attach_explicit (bool sys_rep = false);

    // Note that while attach*() can be called on the attached database,
    // detach_all() should only be called on the main database.
    //
    void
    detach_all ();

    database&
    main_database ()
    {
      return static_cast<database&> (odb::sqlite::database::main_database ());
    }

    // Return true if this is the main database.
    //
    bool
    main ();

    // Return the explicit links and the self-link (comes first) if the main
    // database has been created with the pre_attach flag set to true and an
    // empty list otherwise.
    //
    linked_configs&
    explicit_links ()
    {
      return explicit_links_;
    }

    // By default attach and cache the implicitly linked configuration
    // databases on the first call and return them along with the self-link
    // (comes first), silently skipping the dangling links. If attach is
    // false, then return an empty list if links were not yet cached by this
    // function's previous call.
    //
    // Note that we skip dangling links without any warning since they can be
    // quite common. Think of a shared host configuration with a bunch of
    // implicitly linked configurations, which are removed and potentially
    // recreated later during the host configuration lifetime. Note however,
    // that we remove the dangling implicit links during migration (see
    // migrate() on details).
    //
    // Also note that for implicitly linked configurations the link
    // information (id, etc) is useless, thus we only return the databases
    // rather than the link information.
    //
    linked_databases&
    implicit_links (bool attach = true, bool sys_rep = false);

    // Return configurations of potential dependencies of packages selected in
    // the current configuration.
    //
    // Specifically, return the self-link (comes first if included) and
    // explicitly linked databases recursively, including them into the
    // resulting list according to the following rules:
    //
    // - If dependency name and type are not specified, then return
    //   configurations of all dependencies (runtime and build-time). In this
    //   case include configurations of the linking configuration type and the
    //   host and build2 types and do not descended into links of different
    //   types.
    //
    //   So, for example, for the following (not very sensible) link chain
    //   only the cfg1 and cfg2 configurations are included. The cfg3 type is
    //   not host and differs from type of cfg2 which links it and thus it is
    //   not included.
    //
    //   cfg1 (this, target) -> cfg2 (host) -> cfg3 (target)
    //
    // - If buildtime is false, then return configurations of only runtime
    //   dependencies, regardless of the dependency name. In this case include
    //   configurations of only the linking configuration type and do not
    //   descend into links of different types.
    //
    //   So for the above link chain only cfg1 configuration is included.
    //
    // - If buildtime is true, then return configurations of only build-time
    //   dependencies, suitable for building the specified dependency. In this
    //   case include configurations of only the build2 type for a build2
    //   module (named as libbuild2-*) and of the host type otherwise. Only
    //   descend into links of the same type and the appropriate dependency
    //   type (host or build2, depending on the dependency name).
    //
    //   So for the above link chain only cfg2 configuration is included for a
    //   build-time dependency foo and none for libbuild2-foo.
    //
    linked_databases
    dependency_configs ();

    linked_databases
    dependency_configs (const package_name& dependency_name, bool buildtime);

    // Return configurations of potential dependents of packages selected in
    // the current configuration.
    //
    // Specifically, return the implicitly linked configuration databases
    // recursively, including the self-link (comes first). Only include a
    // linked configuration into the resulting list if it is of the same type
    // as the linking configuration or the linking configuration is of the
    // host or build2 type (think of searching through the target
    // configurations for dependents of a build-time dependency in host
    // configuration).
    //
    linked_databases
    dependent_configs (bool sys_rep = false);

    // The following find_*() functions assume that the main database has been
    // created with the pre_attach flag set to true.
    //

    // Return the self reference if the id is 0. Otherwise, return the
    // database of an explicitly linked configuration with the specified link
    // id and issue diagnostics and fail if no link is found.
    //
    database&
    find_attached (uint64_t id);

    // Return the self reference if this is the current configuration
    // name. Otherwise, return the database of an explicitly linked
    // configuration with the specified name and issue diagnostics and fail if
    // no link is found.
    //
    database&
    find_attached (const std::string& name);

    // Return the dependency configuration with the specified uuid and issue
    // diagnostics and fail if not found.
    //
    database&
    find_dependency_config (const uuid_type&);

    // Return an empty string for the main database and the original
    // configuration directory path in the `[<dir>]` form otherwise.
    //
    // NOTE: remember to update pkg_command_vars::string() if changing the
    // format.
    //
    std::string
    string ();

    // Verify that the link information (uuid, type, etc) matches the linked
    // configuration. Issue diagnostics and fail if that's not the case.
    //
    void
    verify_link (const configuration&, database&);

    // Set the specified tracer for the whole linked databases cluster.
    //
    using tracer_type = odb::tracer;

    void
    tracer (tracer_type*);

    void
    tracer (tracer_type& t) {tracer (&t);}

    using odb::sqlite::database::tracer;

  public:
    // Cached configuration information.
    //
    uuid_type             uuid;
    optional<std::string> name;
    std::string           type;

    // Absolute and normalized configuration directory path. In particular, it
    // is used as the configuration database identity.
    //
    dir_path config;

    // For the main database, this is the original configuration directory
    // path as specified by the user on the command line and `./` if
    // unspecified. For other (linked) databases, it is the absolute
    // configuration path if the main database's original configuration path
    // is absolute and the path relative to the current directory otherwise.
    // This is used in diagnostics.
    //
    dir_path config_orig;

    // Per-configuration system repository (only loaded if sys_rep constructor
    // argument is true).
    //
    optional<bpkg::system_repository> system_repository;

  private:
    struct impl;

    // Create/open main database.
    //
    database (const dir_path& cfg,
              configuration* create,
              odb::tracer&,
              bool pre_attach,
              bool sys_rep,
              const dir_paths& pre_link);

    // Create attached database.
    //
    database (impl*,
              const dir_path& cfg,
              std::string schema,
              bool sys_rep);

    // If necessary, migrate this database and all the linked (both explicitly
    // and implicitly) databases, recursively. Leave the linked databases
    // attached. Must be called inside the transaction.
    //
    // Note that since the whole linked databases cluster is migrated at once,
    // it is assumed that if migration is unnecessary for this database then
    // it is also unnecessary for its linked databases. By this reason, we
    // also drop the dangling implicit links rather than skip them, as we do
    // for normal operations (see implicit_links () for details).
    //
    void
    migrate ();

    // Cache the configuration information.
    //
    void
    cache_config (const uuid_type&,
                  optional<std::string> name,
                  std::string type);

    // Note: must be called inside the transaction.
    //
    void
    load_system_repository ();

    // Add the configuration path to the BPKG_OPEN_CONFIGS environment
    // variable which contains a list of the space-separated double-quoted
    // absolute directory paths. Optionally, reset the list to this database's
    // single path.
    //
    void
    add_env (bool reset = false) const;

    // Common implementation for the public overloads.
    //
    linked_databases
    dependency_configs (optional<bool> buildtime, const std::string& type);

    impl* impl_;

    linked_configs   explicit_links_;
    linked_databases implicit_links_;
  };

  // NOTE: remember to update config_package comparison operators and
  // compare_lazy_ptr if changing the database comparison operators.
  //
  // Note that here we use the database address as the database identity since
  // we don't suppose two database instances for the same configuration to
  // exist simultaneously due to the EXCLUSIVE locking mode (see database
  // constructor for details).
  //
  inline bool
  operator== (const database& x, const database& y)
  {
    return &x == &y;
  }

  inline bool
  operator!= (const database& x, const database& y)
  {
    return !(x == y);
  }

  inline bool
  operator< (const database& x, const database& y)
  {
    // Note that if we ever need the ordering to be consistent across runs,
    // then we can compare the config paths or uuids.
    //
    return &x < &y;
  }

  inline ostream&
  operator<< (ostream& os, const database& db)
  {
    string s (const_cast<database&> (db).string ());

    if (!s.empty ())
      os << ' ' << s;

    return os;
  }

  // Verify that a string is a valid configuration name, that is non-empty,
  // containing only alpha-numeric characters, '_', '-' (except for the first
  // character which can only be alphabetic or '_'). Issue diagnostics and
  // fail if that's not the case.
  //
  void
  validate_configuration_name (const string&, const char* what);

  // The build-time dependency configuration types.
  //
  // Note that these are also used as private configuration names.
  //
  extern const string host_config_type;
  extern const string build2_config_type;

  // Return the configuration type suitable for building the specified
  // build-time dependency: `build2` for build2 modules and `host` for others.
  //
  const string&
  buildtime_dependency_type (const package_name&);

  // Return the configuration type suitable for building a dependency of the
  // dependent in the specified configuration: `build2` for build2 modules,
  // `host` for other (regular) build-time dependencies, and the dependent
  // configuration type for the runtime dependencies.
  //
  inline const string&
  dependency_type (database& dependent_db,
                   const package_name& dependency_name,
                   bool buildtime)
  {
    return buildtime
           ? buildtime_dependency_type (dependency_name)
           : dependent_db.type;
  }

  // Transaction wrapper that allow the creation of dummy transactions (start
  // is false) that in reality use an existing transaction.
  //
  // Note that there can be multiple databases attached to the main database
  // and normally a transaction object is passed around together with a
  // specific database. Thus, we don't provide the database accessor function,
  // so that the database is always chosen deliberately.
  //
  class transaction
  {
  public:
    using database_type = bpkg::database;

    explicit
    transaction (database_type& db, bool start = true)
        : start_ (start), t_ () // Finalized.
    {
      if (start)
        t_.reset (db.begin_exclusive ()); // See locking_mode for details.
    }

    void
    commit ()
    {
      if (start_)
      {
        t_.commit ();
        start_ = false;
      }
    }

    void
    rollback ()
    {
      if (start_)
      {
        t_.rollback ();
        start_ = false;
      }
    }

    void
    start (database_type& db)
    {
      assert (!start_);

      start_ = true;
      t_.reset (db.begin_exclusive ());
    }

    static bool
    has_current ()
    {
      return odb::sqlite::transaction::has_current ();
    }

  private:
    bool start_;
    odb::sqlite::transaction t_;
  };

  struct tracer_guard
  {
    tracer_guard (database& db, tracer& t)
        : db_ (db), t_ (db.tracer ()) {db.tracer (t);}
    ~tracer_guard () {db_.tracer (t_);}

  private:
    database& db_;
    odb::tracer* t_;
  };

  // Range-based for-loop iteration over query result that returns
  // object pointers. For example:
  //
  // for (shared_ptr<object> o: pointer_result (db.query<object> (...)))
  //
  template <typename R>
  class pointer_result_range
  {
    R r_;

  public:
    pointer_result_range (R&& r): r_ (forward<R> (r)) {}

    using base_iterator = typename std::remove_reference<R>::type::iterator;

    struct iterator: base_iterator
    {
      iterator () = default;

      explicit
      iterator (base_iterator i): base_iterator (move (i)) {}

      typename base_iterator::pointer_type
      operator* () {return this->load ();}
    };

    iterator begin () {return iterator (r_.begin ());}
    iterator end () {return iterator (r_.end ());}
  };

  template <typename R>
  inline pointer_result_range<R>
  pointer_result (R&& r)
  {
    return pointer_result_range<R> (forward<R> (r));
  }

  // Note that lazy_shared_ptr and lazy_weak_ptr are defined in types.hxx.
  //
  template <typename T>
  inline database& lazy_shared_ptr<T>::
  database () const
  {
    return static_cast<bpkg::database&> (base_type::database ());
  }

  template <typename T>
  inline database& lazy_weak_ptr<T>::
  database () const
  {
    return static_cast<bpkg::database&> (base_type::database ());
  }
}

#endif // BPKG_DATABASE_HXX
