// file      : bpkg/rep-remove.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-remove.hxx>

#include <set>
#include <algorithm> // find()

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

    for (const auto& rc: db.query<repository_complement_dependent> (
           query<repository_complement_dependent>::complement::name == nm))
    {
      const shared_ptr<repository>& r (rc);
      if (r->name.empty () /* Root? */ || reachable (db, r, traversed))
        return true;
    }

    for (const auto& rd: db.query<repository_prerequisite_dependent> (
           query<repository_prerequisite_dependent>::prerequisite::name == nm))
    {
      // Note that the root repository has no prerequisites.
      //
      if (reachable (db, rd, traversed))
        return true;
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
  rep_remove_package_locations (transaction& t, const string& name)
  {
    database& db (t.database ());

    for (const auto& rp: db.query<repository_package> (
           query<repository_package>::repository::name == name))
    {
      const shared_ptr<available_package>& p (rp);
      vector<package_location>& ls (p->locations);

      for (auto i (ls.cbegin ()); i != ls.cend (); )
      {
        if (i->repository.object_id () == name)
          i = ls.erase (i);
        else
          ++i;
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
    const string& nm (r->name);
    assert (!nm.empty ());      // Can't be the root repository.

    database& db (t.database ());

    if (reachable (db, r))
      return;

    rep_remove_package_locations (t, nm);

    // Cleanup the repository state if present.
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
        rmdir (sd);
    }

    // Note that it is essential to erase the repository object from the
    // database prior to its complements and prerequisites removal as they
    // must be un-referenced first.
    //
    db.erase (r);

    // Remove dangling complements and prerequisites.
    //
    // Prior to removing a prerequisite/complement we need to make sure it
    // still exists, which may not be the case due to the dependency cycle.
    //
    auto remove = [&c, &db, &t] (const lazy_shared_ptr<repository>& rp)
    {
      shared_ptr<repository> r (db.find<repository> (rp.object_id ()));

      if (r)
        rep_remove (c, t, r);
    };

    for (const lazy_shared_ptr<repository>& cr: r->complements)
    {
      // Remove the complement unless it is the root repository (see
      // rep_fetch() for details).
      //
      if (cr.object_id () != "")
        remove (cr);
    }

    for (const lazy_weak_ptr<repository>& pr: r->prerequisites)
      remove (lazy_shared_ptr<repository> (pr));
  }

  void
  rep_remove_clean (const common_options& o,
                    const dir_path& c,
                    database& db,
                    bool quiet)
  {
    tracer trace ("rep_remove_clean");

    assert (!transaction::has_current ());

    // Clean repositories and available packages. At the end only repositories
    // that were explicitly added by the user and the special root repository
    // should remain.
    //
    {
      // Note that we don't rely on being in session nor create one.
      //
      transaction t (db);

      db.erase_query<available_package> ();

      shared_ptr<repository> root (db.load<repository> (""));
      repository::complements_type& ua (root->complements);

      for (shared_ptr<repository> r: pointer_result (db.query<repository> ()))
      {
        if (r->name == "")
        {
          l5 ([&]{trace << "skipping root";});
        }
        else if (ua.find (lazy_shared_ptr<repository> (db, r)) != ua.end ())
        {
          r->complements.clear ();
          r->prerequisites.clear ();
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
      for (const dir_entry& de: dir_iterator (rd)) // system_error
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

    shared_ptr<repository> root (db.load<repository> (""));
    repository::complements_type& ua (root->complements);

    if (o.all ())
    {
      for (const lazy_shared_ptr<repository>& r: ua)
        repos.push_back (r);
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

            if (u.empty ())
              fail << "empty repository location";

            for (const lazy_shared_ptr<repository>& rp: ua)
            {
              if (rp.load ()->location.url () == u)
              {
                r = rp;
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

    // If we removed all the user-added repositories then no repositories nor
    // packages should stay in the database.
    //
    assert (!ua.empty () ||
            (db.query_value<repository_count> () == 0 &&
             db.query_value<available_package_count> () == 0));

    t.commit ();

    return 0;
  }
}
