// file      : bpkg/rep-mask.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-mask.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/rep-remove.hxx>       // rep_remove_verify()
#include <bpkg/diagnostics.hxx>
#include <bpkg/package-query.hxx>    // repo_configs
#include <bpkg/manifest-utility.hxx> // repository_name()

using namespace std;
using namespace butl;

namespace bpkg
{
  static optional<database_map<strings>> unmasked_repositories;
  static optional<database_map<strings>> unmasked_repository_fragments;

  // Note: defined in rep-remove.cxx.
  //
  void
  rep_remove (database&,
              transaction&,
              const shared_ptr<repository>&,
              bool mask);

  // The overall plan is as follows:
  //
  // - Start the transaction.
  //
  // - Remove all the specified repositories recursively in all the
  //   configurations specified by repo_configs (repos) and/or configurations
  //   specified explicitly via UUIDs (config_uuid_repos).
  //
  // - Collect the remaining repositories and repository fragments as unmasked.
  //
  // - Rollback the transaction.
  //
  // Later on, the rep_masked*() functions will refer to the configuration-
  // specific unmasked repositories and repository fragments lists to decide
  // if the repository/fragment is masked or not in the specific configuration.
  //
  void
  rep_mask (const strings& repos,
            const strings& config_uuid_repos,
            linked_databases& current_configs)
  {
    tracer trace ("rep_mask");

    assert (!repo_configs.empty ());

    database& mdb (repo_configs.front ());
    tracer_guard tg (mdb, trace);

    // Temporary "suspend" session before modifying the database.
    //
    session* sess (session::current_pointer ());
    if (sess != nullptr)
      session::reset_current ();

    vector<lazy_weak_ptr<repository>> rs;
    vector<bool> found_repos (repos.size(), false);

    transaction t (mdb);

    // Add a repository from a database, suppressing duplicates.
    //
    auto add_repo = [&rs] (database& db, shared_ptr<repository>&& r)
    {
      if (find_if (rs.begin (), rs.end (),
                   [&db, &r] (const lazy_weak_ptr<repository>& lr)
                   {
                     return lr.database () == db && lr.object_id () == r->name;
                   }) == rs.end ())
        rs.emplace_back (db, move (r));
    };

    // Collect the repositories masked in all configurations.
    //
    for (database& db: repo_configs)
    {
      for (size_t i (0); i != repos.size (); ++i)
      {
        // Add a repository, suppressing duplicates, and mark it as found.
        //
        auto add = [&db,
                    &found_repos, i,
                    &add_repo] (shared_ptr<repository>&& r)
        {
          add_repo (db, move (r));
          found_repos[i] = true;
        };

        const string& rp (repos[i]);

        if (repository_name (rp))
        {
          if (shared_ptr<repository> r = db.find<repository> (rp))
            add (move (r));
        }
        else
        {
          using query = query<repository>;

          // Verify that the repository URL is not misspelled or empty.
          //
          try
          {
            repository_url u (rp);
            assert (!u.empty ());
          }
          catch (const invalid_argument& e)
          {
            fail << "repository '" << rp << "' cannot be masked: "
                 << "invalid repository location: " << e;
          }

          for (shared_ptr<repository> r:
                 pointer_result (
                   db.query<repository> (query::location.url == rp)))
            add (move (r));
        }
      }
    }

    // Fail if any of the specified repositories is not found in any database.
    //
    for (size_t i (0); i != repos.size (); ++i)
    {
      if (!found_repos[i])
        fail << "repository '" << repos[i] << "' cannot be masked: not found";
    }

    // Collect the repositories masked in specific configurations.
    //
    for (const string& cr: config_uuid_repos)
    {
      auto bad = [&cr] (const string& d)
      {
        fail << "configuration repository '" << cr << "' cannot be masked: "
             << d;
      };

      size_t p (cr.find ('='));

      if (p == string::npos)
        bad ("missing '='");

      uuid uid;
      string uid_str (cr, 0, p);

      try
      {
        uid = uuid (uid_str);
      }
      catch (const invalid_argument& e)
      {
        bad ("invalid configuration uuid '" + uid_str + "': " + e.what ());
      }

      database* db (nullptr);

      for (database& cdb: current_configs)
      {
        if ((db = cdb.try_find_dependency_config (uid)) != nullptr)
          break;
      }

      if (db == nullptr)
        bad ("no configuration with uuid " + uid.string () +
             " is linked with "                            +
             (current_configs.size () == 1
              ? mdb.config_orig.representation ()
              : "specified current configurations"));

      string rp (cr, p + 1);

      if (repository_name (rp))
      {
        if (shared_ptr<repository> r = db->find<repository> (rp))
          add_repo (*db, move (r));
        else
          bad ("repository name '" + rp + "' not found in configuration " +
               uid.string ());
      }
      else
      {
        using query = query<repository>;

        // Verify that the repository URL is not misspelled or empty.
        //
        try
        {
          repository_url u (rp);
          assert (!u.empty ());
        }
        catch (const invalid_argument& e)
        {
          bad ("invalid repository location '" + rp + "': " + e.what ());
        }

        bool found (false);
        for (shared_ptr<repository> r:
               pointer_result (
                 db->query<repository> (query::location.url == rp)))
        {
          add_repo (*db, move (r));
          found = true;
        }

        if (!found)
          bad ("repository location '" + rp + "' not found in configuration " +
               uid.string ());
      }
    }

    // First, remove the repository references from the dependent repository
    // fragments. Note that rep_remove() removes the dangling repositories.
    //
    // Note that for efficiency we un-reference all the repositories before
    // starting to delete them.
    //
    for (const lazy_weak_ptr<repository>& r: rs)
    {
      database& db (r.database ());
      const string& nm (r.object_id ());

      // Remove from complements of the dependents.
      //
      for (const auto& rf: db.query<repository_complement_dependent> (
             query<repository_complement_dependent>::complement::name == nm))
      {
        const shared_ptr<repository_fragment>& f (rf);
        repository_fragment::dependencies& cs (f->complements);

        auto i (cs.find (r));
        assert (i != cs.end ());

        cs.erase (i);
        db.update (f);
      }

      // Remove from prerequisites of the dependents.
      //
      for (const auto& rf:
             db.query<repository_prerequisite_dependent> (
               query<repository_prerequisite_dependent>::prerequisite::name ==
               nm))
      {
        const shared_ptr<repository_fragment>& f (rf);
        repository_fragment::dependencies& ps (f->prerequisites);

        auto i (ps.find (r));
        assert (i != ps.end ());

        ps.erase (i);
        db.update (f);
      }
    }

    // Remove the now dangling repositories.
    //
    for (const lazy_weak_ptr<repository>& r: rs)
      rep_remove (r.database (), t, r.load (), true /* mask */);

    // Collect the repositories and fragments which have remained after the
    // removal.
    //
    unmasked_repositories         = database_map<strings> ();
    unmasked_repository_fragments = database_map<strings> ();

    for (database& db: repo_configs)
    {
      // While at it, verify that the repository information has stayed
      // consistent after the potential repository removals.
      //
      // Note that rep_remove() doesn't remove the available packages in the
      // mask mode and thus we don't verify them.
      //
#ifndef NDEBUG
      rep_remove_verify (db, t, false /* verify_packages */);
#endif

      // Add the repository location canonical name to the database-specific
      // unmasked repositories or repository fragments lists. Note that
      // repository location is used only for tracing.
      //
      auto add = [&db, &trace] (string&& n,
                                database_map<strings>& m,
                                const repository_location& loc,
                                const char* what)
      {
        auto i (m.find (db));
        if (i == m.end ())
          i = m.insert (db, strings ()).first;

        l4 ([&]{trace << "unmasked " << what << ": '" << n
                      << "' '" << loc.url () << "'" << db;});

        i->second.push_back (move (n));
      };

      for (shared_ptr<repository> r: pointer_result (db.query<repository> ()))
        add (move (r->name),
             *unmasked_repositories,
             r->location,
             "repository");

      for (shared_ptr<repository_fragment> f:
             pointer_result (db.query<repository_fragment> ()))
        add (move (f->name),
             *unmasked_repository_fragments,
             f->location,
             "repository fragment");
    }

    // Rollback the transaction and restore the session, if present.
    //
    t.rollback ();

    if (sess != nullptr)
      session::current_pointer (sess);
  }

  static inline bool
  masked (database& db,
          const string& name,
          const optional<database_map<strings>>& m)
  {
    if (!m)
      return false;

    auto i (m->find (db));
    if (i != m->end ())
    {
      const strings& ns (i->second);
      return find (ns.begin (), ns.end (), name) == ns.end ();
    }

    return true;
  }

  bool
  rep_masked (database& db, const shared_ptr<repository>& r)
  {
    return masked (db, r->name, unmasked_repositories);
  }

  bool
  rep_masked (const lazy_weak_ptr<repository>& r)
  {
    // Should not be transient.
    //
    assert (!(r.lock ().get_eager () != nullptr && !r.loaded ()));

    return masked (r.database (), r.object_id (), unmasked_repositories);
  }

  bool
  rep_masked_fragment (database& db, const shared_ptr<repository_fragment>& f)
  {
    return masked (db, f->name, unmasked_repository_fragments);
  }

  bool
  rep_masked_fragment (const lazy_shared_ptr<repository_fragment>& f)
  {
    // Should not be transient.
    //
    assert (!(f.get_eager () != nullptr && !f.loaded ()));

    return masked (f.database (),
                   f.object_id (),
                   unmasked_repository_fragments);
  }
}
