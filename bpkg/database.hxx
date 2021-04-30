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

  // Derive a custom database class that handles attaching/detaching
  // additional configurations.
  //
  class database: public odb::sqlite::database
  {
  public:
    // Create main database.
    //
    // If pre-associate is not NULL, then this configration is treated as
    // another associated configuration for schema migration purposes.
    //
    database (const dir_path& cfg,
              odb::tracer&,
              bool create = false,
              bool sys_rep = true,
              const dir_path* pre_associate = nullptr);

    ~database ();

    database (database&&) = delete;
    database& operator= (database&&) = delete;

    // Attach another (existing) database. The configuration directory should
    // be absolute and normalized.
    //
    database&
    attach (const dir_path&, bool sys_rep = true);

    // Note that while attach() can be called on the attached database,
    // detach_all() should only be called on the main database.
    //
    void
    detach_all ();

    // Per-configuration system repository (only loaded if sys_rep above is
    // true).
    //
  public:
    bpkg::system_repository system_repository;

  private:
    struct impl;

    // Create attached database.
    //
    database (impl*,
              const dir_path& cfg,
              string schema,
              bool sys_rep);

    void
    load_system_repository ();

    impl* impl_;
  };

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
        : db_ (db), start_ (start), t_ () // Finalized.
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
    database_type& db_;
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
