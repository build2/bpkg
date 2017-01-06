// file      : bpkg/rep-fetch.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-fetch>

#include <bpkg/manifest>

#include <bpkg/auth>
#include <bpkg/fetch>
#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/database>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  static void
  rep_fetch (const configuration_options& co,
             transaction& t,
             const shared_ptr<repository>& r,
             const shared_ptr<repository>& root,
             const string& reason)
  {
    tracer trace ("rep_fetch(rep)");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    const repository_location& rl (r->location);
    l4 ([&]{trace << r->name << " " << rl;});
    assert (rl.absolute () || rl.remote ());

    // The fetch_*() functions below will be quiet at level 1, which
    // can be quite confusing if the download hangs.
    //
    if (verb)
    {
      diag_record dr (text);

      dr << "fetching " << r->name;

      const auto& ua (root->complements);

      if (ua.find (lazy_shared_ptr<repository> (db, r)) == ua.end ())
      {
        assert (!reason.empty ());
        dr << " (" << reason << ")";
      }
    }

    r->fetched = true; // Mark as being fetched.

    // Load the 'repositories' file and use it to populate the prerequisite
    // and complement repository sets.
    //
    pair<repository_manifests, string/*checksum*/> rmc (
      fetch_repositories (co, rl, true));

    repository_manifests& rms (rmc.first);

    bool a (co.auth () != auth::none &&
            (co.auth () == auth::all || rl.remote ()));

    shared_ptr<const certificate> cert;

    if (a)
    {
      cert = authenticate_certificate (
        co, &co.directory (), rms.back ().certificate, rl);

      a = !cert->dummy ();
    }

    // Load the 'packages' file.
    //
    pair<package_manifests, string/*checksum*/> pmc (
      fetch_packages (co, rl, true));

    package_manifests& pms (pmc.first);

    if (rmc.second != pms.sha256sum)
      fail << "repositories manifest file checksum mismatch for "
           << rl.canonical_name () <<
        info << "try again";

    if (a)
    {
      signature_manifest sm (fetch_signature (co, rl, true));

      if (sm.sha256sum != pmc.second)
        fail << "packages manifest file checksum mismatch for "
             << rl.canonical_name () <<
          info << "try again";

      assert (cert != nullptr);
      authenticate_repository (co, &co.directory (), nullopt, *cert, sm, rl);
    }

    for (repository_manifest& rm: rms)
    {
      repository_role rr (rm.effective_role ());

      if (rr == repository_role::base)
        continue; // Entry for this repository.

      // If the location is relative, complete it using this repository
      // as a base.
      //
      if (rm.location.relative ())
      {
        try
        {
          rm.location = repository_location (rm.location, rl);
        }
        catch (const invalid_argument& e)
        {
          fail << "invalid relative repository location '" << rm.location
               << "': " << e <<
            info << "base repository location is " << rl;
        }
      }

      // We might already have this repository in the database.
      //
      shared_ptr<repository> pr (
        db.find<repository> (
          rm.location.canonical_name ()));

      if (pr == nullptr)
      {
        pr = make_shared<repository> (move (rm.location));
        db.persist (pr); // Enter into session, important if recursive.
      }

      // Load the prerequisite repository unless it has already been
      // (or is already being) fetched.
      //
      if (!pr->fetched)
      {
        string reason;
        switch (rr)
        {
        case repository_role::complement:   reason = "complements ";     break;
        case repository_role::prerequisite: reason = "prerequisite of "; break;
        case repository_role::base: assert (false);
        }
        reason += r->name;

        rep_fetch (co, t, pr, root, reason);
      }

      // @@ What if we have duplicated? Ideally, we would like to check
      //    this once and as early as possible. The original idea was to
      //    do it during manifest parsing and serialization. But at that
      //    stage we have no way of completing relative locations (which
      //    is required to calculate canonical names). Current thinking is
      //    that we should have something like rep-verify (similar to
      //    pkg-verify) that performs (potentially expensive) repository
      //    verifications, including making sure prerequisites can be
      //    satisfied from the listed repositories, etc. Perhaps we can
      //    also re-use some of that functionality here. I.e., instead of
      //    calling the "naked" fetch_repositories() above, we will call
      //    a function from rep-verify that will perform extra verifications.
      //
      // @@ Also check for self-prerequisite.
      //
      switch (rr)
      {
      case repository_role::complement:
        {
          l4 ([&]{trace << pr->name << " complement of " << r->name;});
          r->complements.insert (lazy_shared_ptr<repository> (db, pr));
          break;
        }
      case repository_role::prerequisite:
        {
          l4 ([&]{trace << pr->name << " prerequisite of " << r->name;});
          r->prerequisites.insert (lazy_weak_ptr<repository> (db, pr));
          break;
        }
      case repository_role::base:
        assert (false);
      }
    }

    // "Suspend" session while persisting packages to reduce memory
    // consumption.
    //
    session& s (session::current ());
    session::reset_current ();

    for (package_manifest& pm: pms)
    {
      // We might already have this package in the database.
      //
      bool persist (false);

      shared_ptr<available_package> p (
        db.find<available_package> (
          available_package_id (pm.name, pm.version)));

      if (p == nullptr)
      {
        p = make_shared<available_package> (move (pm));
        persist = true;
      }
      else
      {
        // Make sure this is the same package.
        //
        assert (p->sha256sum && !p->locations.empty ()); // Can't be transient.

        if (*pm.sha256sum != *p->sha256sum)
        {
          // All the previous repositories that contain this package have the
          // same checksum (since they passed this test), so we can pick any
          // to show to the user.
          //
          const string& r1 (rl.canonical_name ());
          const string& r2 (p->locations[0].repository.object_id ());

          fail << "checksum mismatch for " << pm.name << " " << pm.version <<
            info << r1 << " has " << *pm.sha256sum <<
            info << r2 << " has " << *p->sha256sum <<
            info << "consider reporting this to the repository maintainers";
        }
      }

      // This repository shouldn't already be in the location set since
      // that would mean it has already been loaded and we shouldn't be
      // here.
      //
      p->locations.push_back (
        package_location {lazy_shared_ptr<repository> (db, r),
                          move (*pm.location)});

      if (persist)
        db.persist (p);
      else
        db.update (p);
    }

    session::current (s); // "Resume".

    // Save the changes to the repository object.
    //
    db.update (r);
  }

  int
  rep_fetch (const rep_fetch_options& o, cli::scanner&)
  {
    tracer trace ("rep_fetch");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    database db (open (c, trace));
    transaction t (db.begin ());
    session s; // Repository dependencies can have cycles.

    shared_ptr<repository> root (db.load<repository> (""));
    const auto& ua (root->complements); // User-added repositories.

    if (ua.empty ())
      fail << "configuration " << c << " has no repositories" <<
        info << "use 'bpkg rep-add' to add a repository";

    // Clean repositories and available packages. At the end only
    // repositories that were explicitly added by the user and the
    // special root repository should remain.
    //
    db.erase_query<available_package> ();

    for (shared_ptr<repository> r: pointer_result (db.query<repository> ()))
    {
      if (r == root)
      {
        l5 ([&]{trace << "skipping root";});
      }
      else if (ua.find (lazy_shared_ptr<repository> (db, r)) != ua.end ())
      {
        l4 ([&]{trace << "cleaning " << r->name;});

        r->complements.clear ();
        r->prerequisites.clear ();
        r->fetched = false;
        db.update (r);
      }
      else
      {
        l4 ([&]{trace << "erasing " << r->name;});
        db.erase (r);
      }
    }

    // Now recursively fetch prerequisite/complement repositories and
    // their packages.
    //
    for (const lazy_shared_ptr<repository>& lp: ua)
    {
      shared_ptr<repository> r (lp.load ());

      if (!r->fetched) // Can already be loaded as a prerequisite/complement.
        rep_fetch (o, t, r, root, ""); // No reason (user-added).
    }

    size_t rcount, pcount;
    if (verb)
    {
      rcount = db.query_value<repository_count> ();
      pcount = db.query_value<available_package_count> ();
    }

    t.commit ();

    if (verb)
      text << pcount << " package(s) in " << rcount << " repository(s)";

    return 0;
  }
}
