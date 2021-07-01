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

  struct associated_config
  {
    uint64_t                    id;
    optional<string>            name;
    reference_wrapper<database> db;   // Needs to be move-assignable.
  };

  // Used for the immediate explicit associations which are normally not many
  // (one entry for the self-association).
  //
  using associated_configs = small_vector<associated_config, 2>;

  // In particular, is used for implicit associations which can potentially be
  // many. Think of a dependency in a shared configuration with dependents in
  // multiple implicitly associated configurations.
  //
  using associated_databases = small_vector<reference_wrapper<database>, 16>;

  // Derive a custom database class that handles attaching/detaching
  // additional configurations.
  //
  class database: public odb::sqlite::database
  {
  public:
    using uuid_type = bpkg::uuid;

    // Create new main database.
    //
    // The specified self-association object is persisted and its uuid and
    // type are cached in the database object.
    //
    // If pre_associate is not NULL, then this configuration is treated as an
    // associated configuration for schema migration purposes. If specified,
    // this path should be absolute and normalized.
    //
    database (const dir_path& cfg,
              const shared_ptr<configuration>& self,
              odb::tracer& tr,
              const dir_path* pre_associate = nullptr)
        : database (cfg, self.get (), tr, false, false, pre_associate)
    {
      assert (self != nullptr);
    }

    // Open existing main database.
    //
    // If configured non-system selected packages can potentially be loaded
    // from this database, then pass true as the pre_attach argument to
    // recursively pre-attach the explicitly associated configuration
    // databases, so that package prerequisites can be loaded from the
    // associated configurations as well (see _selected_package_ref::to_ptr()
    // implementation for details). Note that selected packages are loaded by
    // some functions internally (package_iteration(), etc). Such functions
    // are marked with the 'Note: loads selected packages.' note.
    //
    database (const dir_path& cfg,
              odb::tracer& tr,
              bool pre_attach,
              bool sys_rep = false,
              const dir_path* pre_associate = nullptr)
        : database (cfg, nullptr, tr, pre_attach, sys_rep, pre_associate) {}

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

    // Attach databases of all the explicitly associated configurations,
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

    // Return the explicit associations and the self-association (comes first)
    // if the main database has been created with the pre_attach flag set to
    // true and an empty list otherwise.
    //
    associated_configs&
    explicit_associations ()
    {
      return explicit_associations_;
    }

    // By default attach and cache the implicitly associated configuration
    // databases on the first call and return them along with the self-
    // association (comes first), silently skipping the dangling
    // associations. If attach is false, then return an empty list if
    // associations were not yet cached by this function's previous call.
    //
    // Note that we skip dangling associations without any warning since they
    // can be quite common. Think of a shared host configuration with a bunch
    // of implicitly associated configurations, which are removed and
    // potentially recreated later during the host configuration lifetime.
    // Note however, that we remove the dangling implicit associations during
    // migration (see migrate() on details).
    //
    // Also note that for implicitly associated configurations the association
    // information (id, etc) is useless, thus we only return the databases
    // rather than the association information.
    //
    associated_databases&
    implicit_associations (bool attach = true, bool sys_rep = false);

    // Return configurations of potential dependencies of packages selected in
    // the current configuration.
    //
    // Specifically, return the self-association (comes first if included) and
    // explicitly associated databases recursively, including them into the
    // resulting list according to the following rules:
    //
    // - If buildtime is nullopt, then return configurations of all
    //   dependencies (runtime and build-time). In this case include
    //   configurations of the associating configuration type and the host
    //   type and do not descended into associations of different types.
    //
    //   So, for example, for the following (not very sensible) association
    //   chain only the cfg1 and cfg2 configurations are included. The cfg3
    //   type is not host and differs from type of cfg2 which associates it
    //   and thus it is not included.
    //
    //   cfg1 (this, target) -> cfg2 (host) -> cfg3 (target)
    //
    // - If buildtime is false, then return configurations of only runtime
    //   dependencies. In this case include configurations of only the
    //   associating configuration type and do not descend into associations
    //   of different types.
    //
    //   So for the above association chain only cfg1 configuration is
    //   included.
    //
    // - If buildtime is true, then return configurations of only build-time
    //   dependencies. In this case include configurations of only the host
    //   type and do not descend into associations of different types and the
    //   host type.
    //
    //   So for the above association chain only cfg2 configuration is
    //   included.
    //
    associated_databases
    dependency_configs (optional<bool> buildtime = nullopt);

    // Return configurations of potential dependents of packages selected in
    // the current configuration.
    //
    // Specifically, return the implicitly associated configuration databases
    // recursively, including the self-association (comes first). Only include
    // an associated configuration into the resulting list if it is of the
    // same type as the associating configuration or the associating
    // configuration is of the host type (think of searching through the
    // target configurations for dependents of a build-time dependency in host
    // configuration).
    //
    associated_databases
    dependent_configs (bool sys_rep = false);

    // The following find_*() functions assume that the main database has been
    // created with the pre_attach flag set to true.
    //

    // Return the self reference if the id is 0. Otherwise, return the
    // database of an explicitly associated configuration with the specified
    // association id and issue diagnostics and fail if no association is
    // found.
    //
    database&
    find_attached (uint64_t id);

    // Return the self reference if this is the current configuration
    // name. Otherwise, return the database of an explicitly associated
    // configuration with the specified name and issue diagnostics and fail if
    // no association is found.
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

    // Verify that the association information (uuid, type, etc) matches the
    // associated configuration. Issue diagnostics and fail if that's not the
    // case.
    //
    void
    verify_association (const configuration&, database&);

    // Set the specified tracer for the whole associated databases cluster.
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
    // unspecified. For other (associated) databases, it is the absolute
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
              const dir_path* pre_associate);

    // Create attached database.
    //
    database (impl*,
              const dir_path& cfg,
              std::string schema,
              bool sys_rep);

    // If necessary, migrate this database and all the associated (both
    // explicitly and implicitly) databases, recursively. Leave the associated
    // databases attached. Must be called inside the transaction.
    //
    // Note that since the whole associated databases cluster is migrated at
    // once, it is assumed that if migration is unnecessary for this database
    // then it is also unnecessary for its associated databases. By this
    // reason, we also drop the dangling implicit associations rather than
    // skip them, as we do for normal operations (see implicit_associations ()
    // for details).
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

    impl* impl_;

    associated_configs   explicit_associations_;
    associated_databases implicit_associations_;
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

  inline string
  buildtime_dependency_config_type (const package_name& nm)
  {
    return nm.string ().compare (0, 10, "libbuild2-") == 0 ? "build2" : "host";
  }

  inline string
  dependency_config_type (database& db, const package_name& nm, bool buildtime)
  {
    return buildtime ? buildtime_dependency_config_type (nm) : db.type;
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
