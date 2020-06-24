// file      : bpkg/rep-remove.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-remove.hxx>

#include <set>

#include <libbutl/filesystem.mxx> // dir_iterator

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  // Return true if the repository is reachable from the root repository via
  // the complements or prerequisites chains, recursively.
  //
  // Note that we can end up with a repository dependency cycle via
  // prerequisites. Thus we need to make sure that the repository was not
  // traversed yet.
  //
  using repositories = set<reference_wrapper<const shared_ptr<repository>>,
                           compare_reference_target>;

  static bool
  reachable (database& db,
             const shared_ptr<repository>& r,
             repositories& traversed)
  {
    const string& nm (r->name);
    assert (!nm.empty ());      // Can't be the root repository.

    // We will go upstream until reach the root or traverse through all of the
    // dependent repositories.
    //
    if (!traversed.insert (r).second) // We have already been here.
      return false;

    // Iterate over repository fragments that depend on this repository as a
    // complement.
    //
    for (const auto& rf: db.query<repository_complement_dependent> (
           query<repository_complement_dependent>::complement::name == nm))
    {
      const shared_ptr<repository_fragment>& f (rf);

      if (f->name.empty ()) // Root?
        return true;

      // Iterate over repositories that contain this repository fragment.
      //
      for (const auto& fr: db.query<fragment_repository> (
             query<fragment_repository>::repository_fragment::name == f->name))
      {
        if (reachable (db, fr, traversed))
          return true;
      }
    }

    // Iterate over repository fragments that depend on this repository as a
    // prerequisite.
    //
    for (const auto& rf: db.query<repository_prerequisite_dependent> (
           query<repository_prerequisite_dependent>::prerequisite::name == nm))
    {
      // Note that the root repository fragment has no prerequisites.
      //
      const shared_ptr<repository_fragment>& f (rf);

      // Iterate over repositories that contain this repository fragment.
      //
      for (const auto& fr: db.query<fragment_repository> (
             query<fragment_repository>::repository_fragment::name == f->name))
      {
        if (reachable (db, fr, traversed))
          return true;
      }
    }

    return false;
  }

  static inline bool
  reachable (database& db, const shared_ptr<repository>& r)
  {
    repositories traversed;
    return reachable (db, r, traversed);
  }

  void
  rep_remove_package_locations (transaction& t, const string& fragment_name)
  {
    tracer trace ("rep_remove_package_locations");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    using query = query<repository_fragment_package>;

    for (const auto& rp: db.query<repository_fragment_package> (
           query::repository_fragment::name == fragment_name))
    {
      const shared_ptr<available_package>& p (rp);
      small_vector<package_location, 1>& ls (p->locations);

      for (auto i (ls.cbegin ()); i != ls.cend (); ++i)
      {
        if (i->repository_fragment.object_id () == fragment_name)
        {
          ls.erase (i);
          break;
        }
      }

      if (ls.empty ())
        db.erase (p);
      else
        db.update (p);
    }
  }

  // Remove a directory moving it to the temporary directory first, increasing
  // the chances for the operation to succeed.
  //
  static void
  rmdir (const dir_path& d)
  {
    dir_path td (temp_dir / d.leaf ());

    if (exists (td))
      rm_r (td);

    mv (d, td);
    rm_r (td, true /* dir_itself */, 3, rm_error_mode::warn);
  }

  void
  rep_remove (const dir_path& c,
              transaction& t,
              const shared_ptr<repository>& r)
  {
    assert (!r->name.empty ()); // Can't be the root repository.

    tracer trace ("rep_remove");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    if (reachable (db, r))
      return;

    // Note that it is essential to erase the repository object from the
    // database prior to the repository fragments it contains as they must be
    // un-referenced first.
    //
    db.erase (r);

    // Remove dangling repository fragments.
    //
    for (const repository::fragment_type& fr: r->fragments)
      rep_remove_fragment (c, t, fr.fragment.load ());

    // If there are no repositories stayed in the database then no repository
    // fragments should stay either.
    //
    if (db.query_value<repository_count> () == 0)
      assert (db.query_value<repository_fragment_count> () == 0);

    // Cleanup the repository state if present and there are no more
    // repositories referring this state.
    //
    // Note that this step is irreversible on failure. If something goes wrong
    // we will end up with a state-less fetched repository and the
    // configuration will be broken. Though, this in unlikely to happen, so
    // we will not bother for now.
    //
    // An alternative approach would be to collect all such directories and
    // then remove them after committing the transaction. Though, we still may
    // fail in the middle due to the filesystem error.
    //
    dir_path d (repository_state (r->location));

    if (!d.empty ())
    {
      dir_path sd (c / repos_dir / d);

      if (exists (sd))
      {
        // There is no way to get the list of repositories that share this
        // state other than traversing all repositories of this type.
        //
        bool rm (true);

        using query = query<repository>;

        for (shared_ptr<repository> rp:
               pointer_result (
                 db.query<repository> (
                   query::name != "" &&
                   query::location.type == to_string (r->location.type ()))))
        {
          if (repository_state (rp->location) == d)
          {
            rm = false;
            break;
          }
        }

        if (rm)
          rmdir (sd);
      }
    }
  }

  void
  rep_remove_fragment (const dir_path& c,
                       transaction& t,
                       const shared_ptr<repository_fragment>& rf)
  {
    tracer trace ("rep_remove_fragment");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    // Bail out if the repository fragment is still used.
    //
    using query = query<fragment_repository_count>;

    if (db.query_value<fragment_repository_count> (
          "fragment=" + query::_val (rf->name)) != 0)
      return;

    // Remove the repository fragment from locations of the available packages
    // it contains. Note that this must be done before the repository fragment
    // removal.
    //
    rep_remove_package_locations (t, rf->name);

    // Remove the repository fragment.
    //
    db.erase (rf);

    // If there are no repository fragments stayed in the database then no
    // repositories nor packages should stay either.
    //
    // Note that a repository is removed prior to the removal of fragments it
    // contains (see rep_remove()). Also note that the packages contained in a
    // repository fragment are removed, if this is the only containing
    // fragment, prior to the fragment removal (see above).
    //
    if (db.query_value<repository_fragment_count> () == 0)
    {
      assert (db.query_value<repository_count> ()        == 0);
      assert (db.query_value<available_package_count> () == 0);
    }

    // Remove dangling complements and prerequisites.
    //
    // Prior to removing a prerequisite/complement we need to make sure it
    // still exists, which may not be the case due to the dependency cycle.
    //
    auto remove = [&c, &db, &t] (const lazy_weak_ptr<repository>& rp)
    {
      if (shared_ptr<repository> r = db.find<repository> (rp.object_id ()))
        rep_remove (c, t, r);
    };

    for (const lazy_weak_ptr<repository>& cr: rf->complements)
    {
      // Remove the complement unless it is the root repository (see
      // rep_fetch() for details).
      //
      if (cr.object_id () != "")
        remove (cr);
    }

    for (const lazy_weak_ptr<repository>& pr: rf->prerequisites)
      remove (pr);
  }

  void
  rep_remove_clean (const common_options& o,
                    const dir_path& c,
                    database& db,
                    bool quiet)
  {
    tracer trace ("rep_remove_clean");
    tracer_guard tg (db, trace);

    assert (!transaction::has_current ());

    // Clean repositories, repository fragments and available packages. At the
    // end only repositories that were explicitly added by the user and the
    // special root repository should remain.
    //
    {
      // Note that we don't rely on being in session nor create one.
      //
      transaction t (db);

      db.erase_query<available_package> ();

      db.erase_query<repository_fragment> (
        query<repository_fragment>::name != "");

      shared_ptr<repository_fragment> root (db.load<repository_fragment> (""));
      repository_fragment::dependencies& ua (root->complements);

      for (shared_ptr<repository> r: pointer_result (db.query<repository> ()))
      {
        if (r->name == "")
          l5 ([&]{trace << "skipping root repository";});
        else if (ua.find (lazy_weak_ptr<repository> (db, r)) != ua.end ())
        {
          r->fragments.clear ();
          db.update (r);

          if (verb >= (quiet ? 2 : 1) && !o.no_result ())
            text << "cleaned " << r->name;
        }
        else
        {
          l4 ([&]{trace << "erasing " << r->name;});
          db.erase (r);
        }
      }

      t.commit ();
    }

    // Remove repository state subdirectories.
    //
    dir_path rd (c / repos_dir);

    try
    {
      for (const dir_entry& de: dir_iterator (rd, false /* ignore_dangling */))
      {
        if (de.ltype () == entry_type::directory)
          rmdir (rd / path_cast<dir_path> (de.path ()));
      }
    }
    catch (const system_error& e)
    {
      fail << "unable to scan directory " << rd << ": " << e;
    }
  }

  int
  rep_remove (const rep_remove_options& o, cli::scanner& args)
  {
    tracer trace ("rep_remove");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    // Check that options and arguments are consistent.
    //
    {
      diag_record dr;

      if (o.clean ())
      {
        if (o.all ())
          dr << fail << "both --clean and --all|-a specified";
        else if (args.more ())
          dr << fail << "both --clean and repository argument specified";
      }
      else if (o.all ())
      {
        if (args.more ())
          dr << fail << "both --all|-a and repository argument specified";
      }
      else if (!args.more ())
        dr << fail << "repository name or location argument expected";

      if (!dr.empty ())
        dr << info << "run 'bpkg help rep-remove' for more information";
    }

    database db (open (c, trace));

    // Clean the configuration if requested.
    //
    if (o.clean ())
    {
      rep_remove_clean (o, c, db, false /* quiet */);
      return 0;
    }

    // Remove the specified repositories.
    //
    // Build the list of repositories the user wants removed.
    //
    vector<lazy_shared_ptr<repository>> repos;

    transaction t (db);
    session s; // Repository dependencies can have cycles.

    shared_ptr<repository_fragment> root (db.load<repository_fragment> (""));
    repository_fragment::dependencies& ua (root->complements);

    if (o.all ())
    {
      for (const lazy_weak_ptr<repository>& r: ua)
        repos.push_back (lazy_shared_ptr<repository> (r));
    }
    else
    {
      while (args.more ())
      {
        // Try to map the argument to a user-added repository.
        //
        lazy_shared_ptr<repository> r;
        string a (args.next ());

        if (repository_name (a))
        {
          lazy_shared_ptr<repository> rp (db, a);

          // Note: we report repositories we could not find for both cases
          // below.
          //
          if (ua.find (rp) != ua.end ())
            r = move (rp);
        }
        else
        {
          // Note that we can't obtain the canonical name by creating the
          // repository location object as that would require the repository
          // type, which is potentially impossible to guess at this stage. So
          // lets just construct the repository URL and search for it among
          // user-added repositories. The linear search should be fine as we
          // don't expect too many of them.
          //
          try
          {
            repository_url u (a);
            assert (!u.empty ());

            for (const lazy_weak_ptr<repository>& rp: ua)
            {
              if (rp.load ()->location.url () == u)
              {
                r = lazy_shared_ptr<repository> (rp);
                break;
              }
            }
          }
          catch (const invalid_argument& e)
          {
            fail << "invalid repository location '" << a << "': " << e;
          }
        }

        if (r == nullptr)
          fail << "repository '" << a << "' does not exist in this "
               << "configuration";

        // Suppress duplicates.
        //
        if (find (repos.begin (), repos.end (), r) == repos.end ())
          repos.emplace_back (move (r));
      }
    }

    // Remove the repository references from the root.
    //
    // Note that for efficiency we un-reference all the top-level repositories
    // before starting to delete them.
    //
    for (const lazy_shared_ptr<repository>& r: repos)
      ua.erase (r);

    db.update (root);

    // Remove the dangling repositories from the database, recursively.
    //
    for (const lazy_shared_ptr<repository>& r: repos)
    {
      rep_remove (c, t, r.load ());

      if (verb && !o.no_result ())
        text << "removed " << r.object_id ();
    }

    // If the --all option is specified then no user-added repositories should
    // remain.
    //
    assert (!o.all () || ua.empty ());

    // If we removed all the user-added repositories then no repositories,
    // repository fragments or packages should stay in the database.
    //
    assert (!ua.empty () ||
            (db.query_value<repository_count> () == 0 &&
             db.query_value<repository_fragment_count> () == 0 &&
             db.query_value<available_package_count> () == 0));

    t.commit ();

    return 0;
  }
}
