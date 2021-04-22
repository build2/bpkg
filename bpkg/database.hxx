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
    uint64_t id;
    reference_wrapper<database> db; // Needs to be move-assignable.
  };

  using associated_configs = small_vector<associated_config, 1>;

  // Derive a custom database class that handles attaching/detaching
  // additional configurations.
  //
  class database: public odb::sqlite::database
  {
  public:
    using uuid_type = bpkg::uuid;

    // Create main database.
    //
    // The specified self configuration object is persisted (auto-generating
    // the configuration id if absent) and the information it contains is
    // cached in the database object.
    //
    database (const dir_path& cfg,
              const shared_ptr<configuration>& sc,
              odb::tracer& tr)
        : database (cfg, sc.get (), tr, false, false, nullptr)
    {
      assert (sc != nullptr);
    }

    // Open main database.
    //
    // If configured non-system packages can potentially be loaded from this
    // database, then pass true as the pre_attach argument to pre-attach the
    // explicitly associated configuration databases, recursively (see
    // _selected_package_ref::to_ptr() implementation for details on such a
    // requirement). Note that a selected package can be loaded internally by
    // some functions (package_iteration(), etc).
    //
    // If pre-associate is not NULL (should be absolute and normalized), then
    // this configuration is treated as another associated configuration for
    // schema migration purposes.
    //
    database (const dir_path& cfg,
              odb::tracer& tr,
              bool pre_attach,
              bool sys_rep = true,
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
    attach (const dir_path&, bool sys_rep = true);

    // Return the self reference if the id is 0 or uuid is the self id.
    // Otherwise, find by id or uuid the attached database among explicitly
    // associated configurations and issue diagnostics and fail if not found.
    //
    database&
    find_attached (uint64_t);

    database&
    find_attached (const uuid_type&);

    // Note that while attach() can be called on the attached database,
    // detach_all() should only be called on the main database.
    //
    void
    detach_all ();

    database&
    main_database ()
    {
      return static_cast<database&> (odb::sqlite::database::main_database ());
    }

    // Return the configuration explicit associations. Assumes that the main
    // database has been created with the pre_attach flag set to true.
    //
    associated_configs&
    explicit_associations ()
    {
      assert (explicit_associations_);
      return *explicit_associations_;
    }

    // Return the configuration implicit associations, including the self
    // configuration. Load and cache the information on the first call.
    //
    associated_configs&
    implicit_associations ();

    // Set the specified tracer for the whole associated databases cluster.
    //
    using tracer_type = odb::tracer;

    void
    tracer (tracer_type*);

    void
    tracer (tracer_type& t) {tracer (&t);}

    using odb::sqlite::database::tracer;

  public:
    uuid_type uuid;
    string type;

    dir_path config; // Absolute and normalized configuration directory.

    // Per-configuration system repository (only loaded if sys_rep above is
    // true).
    //
    bpkg::system_repository system_repository;

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
              string schema,
              bool sys_rep);

    // If necessary, migrate this database and all the associated databases,
    // recursively. Leave associated databases attached. Must be called inside
    // the transaction.
    //
    // Note that since the whole associated databases cluster is migrated at
    // once, it is assumed that if migration is unnecessary for a database
    // then it is also unnecessary for its associated databases.
    //
    void
    migrate ();

    void
    cache_config (const configuration&);

    // Must be called inside the transaction.
    //
    void
    load_system_repository ();

    void
    attach_explicit (bool sys_rep);

    impl* impl_;

    optional<associated_configs> explicit_associations_;
    associated_configs           implicit_associations_;
  };

  // @@ EC
  //
  inline bool
  operator== (const database& x, const database& y)
  {
    return &x == &y;
  }

  inline bool
  operator< (const database& x, const database& y)
  {
    // Note that if we ever need consistent configuration ordering, then we
    // can compare the config paths or uuids.
    //
    return &x < &y;
  }

  // Transaction wrapper that allow the creation of dummy transactions (start
  // is false) that in reality use an existing transaction.
  //
  // Note that there can be multiple databases attached to the primary one and
  // normally a transaction object is passed around together with a specific
  // database. Thus, we don't provide the database accessor function, so that
  // the database is always chosen deliberately.
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
        t_.commit ();
    }

    void
    rollback ()
    {
      if (start_)
        t_.rollback ();
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
}

#endif // BPKG_DATABASE_HXX
