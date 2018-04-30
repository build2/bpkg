// file      : bpkg/rep-fetch.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-fetch.hxx>

#include <set>
#include <algorithm> // equal()

#include <libbutl/manifest-parser.mxx>

#include <bpkg/auth.hxx>
#include <bpkg/fetch.hxx>
#include <bpkg/rep-add.hxx>
#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/rep-remove.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  // The fetch operation failure may result in mismatch of the (rolled back)
  // repository database state and the repository filesystem state. Restoring
  // the filesystem state on failure would require making copies which seems
  // unnecessarily pessimistic. So instead, we will revert the repository
  // state to the clean state as if repositories were added but never fetched
  // (see rep_remove_clean() for more details).
  //
  // The following flag is set by the rep_fetch_*() functions when they are
  // about to change the repository filesystem state. That, in particular,
  // means that the flag will be set even if the subsequent fetch operation
  // fails, and so the caller can rely on it while handling the thrown
  // exception. The flag must be reset by such a caller prior to the
  // rep_fetch_*() call.
  //
  static bool filesystem_state_changed;

  static rep_fetch_data
  rep_fetch_pkg (const common_options& co,
                 const dir_path* conf,
                 const repository_location& rl,
                 bool ignore_unknown)
  {
    // First fetch the repositories list and authenticate the base's
    // certificate.
    //
    pair<pkg_repository_manifests, string /* checksum */> rmc (
      pkg_fetch_repositories (co, rl, ignore_unknown));

    rep_fetch_data::fragment fr;
    fr.repositories = move (rmc.first);

    bool a (co.auth () != auth::none &&
            (co.auth () == auth::all || rl.remote ()));

    shared_ptr<const certificate> cert;
    optional<string> cert_pem (move (fr.repositories.back ().certificate));

    if (a)
    {
      cert = authenticate_certificate (co, conf, cert_pem, rl);
      a = !cert->dummy ();
    }

    // Now fetch the packages list and make sure it matches the repositories
    // we just fetched.
    //
    pair<pkg_package_manifests, string /* checksum */> pmc (
      pkg_fetch_packages (co, rl, ignore_unknown));

    pkg_package_manifests& pms (pmc.first);

    if (rmc.second != pms.sha256sum)
      fail << "repositories manifest file checksum mismatch for "
           << rl.canonical_name () <<
        info << "try again";

    fr.packages = move (pms);

    if (a)
    {
      signature_manifest sm (
        pkg_fetch_signature (co, rl, true /* ignore_unknown */));

      if (sm.sha256sum != pmc.second)
        fail << "packages manifest file checksum mismatch for "
             << rl.canonical_name () <<
          info << "try again";

      assert (cert != nullptr);
      authenticate_repository (co, conf, cert_pem, *cert, sm, rl);
    }

    return rep_fetch_data {{move (fr)}, move (cert_pem), move (cert)};
  }

  template <typename M>
  static M
  parse_manifest (const path& f,
                  bool iu,
                  const repository_location& rl,
                  const optional<string>& fragment) // Used for diagnostics.
  {
    try
    {
      ifdstream ifs (f);
      manifest_parser mp (ifs, f.string ());
      return M (mp, iu);
    }
    catch (const manifest_parsing& e)
    {
      diag_record dr (fail (e.name, e.line, e.column));

      dr << e.description <<
        info << "repository " << rl;

      if (fragment)
        dr << ' ' << *fragment;

      dr << endf;
    }
    catch (const io_error& e)
    {
      fail << "unable to read from " << f << ": " << e << endf;
    }
  }

  // Parse the repositories manifest file if exists. Otherwise return the
  // repository manifest list containing the only (trivial) base repository.
  //
  template <typename M>
  static M
  parse_repository_manifests (const path& f,
                              bool iu,
                              const repository_location& rl,
                              const optional<string>& fragment)
  {
    M r;
    if (exists (f))
      r = parse_manifest<M> (f, iu, rl, fragment);
    else
      r.emplace_back (repository_manifest ()); // Add the base repository.

    return r;
  }

  // Parse the package directories manifest file if exists. Otherwise treat
  // the current directory as a package and return the manifest list with the
  // single entry referencing this package.
  //
  template <typename M>
  static M
  parse_directory_manifests (const path& f,
                             bool iu,
                             const repository_location& rl,
                             const optional<string>& fragment)
  {
    M r;
    if (exists (f))
      r = parse_manifest<M> (f, iu, rl, fragment);
    else
    {
      r.push_back (package_manifest ());
      r.back ().location = current_dir;
    }

    return r;
  }

  // Parse package manifests referenced by the package directory manifests.
  //
  static vector<package_manifest>
  parse_package_manifests (const common_options& co,
                           const dir_path& repo_dir,
                           vector<package_manifest>&& sms,
                           bool iu,
                           const repository_location& rl,
                           const optional<string>& fragment) // For diagnostics.
  {
    vector<package_manifest> r;
    r.reserve (sms.size ());

    for (package_manifest& sm: sms)
    {
      assert (sm.location);

      auto package_info = [&sm, &rl, &fragment] (diag_record& dr)
      {
        dr << "package ";

        if (!sm.location->current ())
          dr << "'" << sm.location->string () << "' "; // Strip trailing '/'.

        dr << "in repository " << rl;

        if (fragment)
          dr << ' ' << *fragment;
      };

      auto failure = [&package_info] (const char* desc)
      {
        diag_record dr (fail);
        dr << desc << " for ";
        package_info (dr);
      };

      dir_path d (repo_dir / path_cast<dir_path> (*sm.location));
      d.normalize (); // In case location is './'.

      path f (d / manifest_file);
      if (!exists (f))
        failure ("no manifest file");

      try
      {
        ifdstream ifs (f);
        manifest_parser mp (ifs, f.string ());
        package_manifest m (mp, iu);

        // Save the package manifest, preserving its location.
        //
        m.location = move (*sm.location);
        sm = move (m);
      }
      catch (const manifest_parsing& e)
      {
        diag_record dr (fail (e.name, e.line, e.column));
        dr << e.description << info;
        package_info (dr);
      }
      catch (const io_error& e)
      {
        fail << "unable to read from " << f << ": " << e;
      }

      // Fix-up the package version.
      //
      optional<version> v (package_version (co, d));

      if (v)
        sm.version = move (*v);

      r.emplace_back (move (sm));
    }

    return r;
  }

  static rep_fetch_data
  rep_fetch_dir (const common_options& co,
                 const repository_location& rl,
                 bool ignore_unknown)
  {
    assert (rl.absolute ());

    dir_path rd (path_cast<dir_path> (rl.path ()));

    rep_fetch_data::fragment fr;

    fr.repositories = parse_repository_manifests<dir_repository_manifests> (
      rd / repositories_file,
      ignore_unknown,
      rl,
      string () /* fragment */);

    dir_package_manifests pms (
      parse_directory_manifests<dir_package_manifests> (
        rd / packages_file,
        ignore_unknown,
        rl,
        string () /* fragment */));

    fr.packages = parse_package_manifests (co,
                                           rd,
                                           move (pms),
                                           ignore_unknown,
                                           rl,
                                           string () /* fragment */);

    return rep_fetch_data {{move (fr)},
                           nullopt /* cert_pem */,
                           nullptr /* certificate */};
  }

  static rep_fetch_data
  rep_fetch_git (const common_options& co,
                 const dir_path* conf,
                 const repository_location& rl,
                 bool ignore_unknown)
  {
    if (conf != nullptr && conf->empty ())
      conf = dir_exists (bpkg_dir) ? &current_dir : nullptr;

    assert (conf == nullptr || !conf->empty ());

    dir_path sd (repository_state (rl));

    auto_rmdir rm (temp_dir / sd);
    dir_path& td (rm.path);

    if (exists (td))
      rm_r (td);

    // If the git repository directory already exists, then we are fetching
    // an already existing repository, moved to the temporary directory first.
    // Otherwise, we initialize the repository in the temporary directory.
    //
    // In the first case also set the filesystem_state_changed flag since we
    // are modifying the repository filesystem state.
    //
    // In the future we can probably do something smarter about the flag,
    // keeping it unset unless the repository state directory is really
    // changed.
    //
    dir_path rd;
    bool init (true);

    if (conf != nullptr)
    {
      rd = *conf / repos_dir / sd;

      if (exists (rd))
      {
        mv (rd, td);
        filesystem_state_changed = true;
        init = false;
      }
    }

    // Initialize a new repository in the temporary directory.
    //
    if (init)
      git_init (co, rl, td);

    // Fetch the repository in the temporary directory.
    //
    // Go through fetched commits, checking them out and collecting the
    // prerequisite repositories and packages.
    //
    // For each checked out commit:
    //
    // - If repositories.manifest file doesn't exist, then synthesize the
    //   repository list with just the base repository.
    //
    // - Save the repositories into the resulting list.
    //
    //   @@ Currently we just save ones from the first commit, assuming them
    //      to be the same for others. However, this is not very practical
    //      and must be fixed.
    //
    // - If packages.manifest file exists then load it into the "skeleton"
    //   packages list. Otherwise, synthesize it with the single:
    //
    //   location: ./
    //
    // - If any of the package locations point to non-existent directory, then
    //   assume it to be in a submodule and checkout submodules, recursively.
    //
    // - For each package location parse the package manifest and add it to
    //   the resulting list.
    //
    rep_fetch_data r;

    for (git_fragment& gf: git_fetch (co, rl, td))
    {
      git_checkout (co, td, gf.commit);

      rep_fetch_data::fragment fr;
      fr.id            = move (gf.commit);
      fr.friendly_name = move (gf.friendly_name);

      // Parse repository manifests.
      //
      fr.repositories = parse_repository_manifests<git_repository_manifests> (
          td / repositories_file,
          ignore_unknown,
          rl,
          fr.friendly_name);

      // Parse package skeleton manifests.
      //
      git_package_manifests pms (
        parse_directory_manifests<git_package_manifests> (
          td / packages_file,
          ignore_unknown,
          rl,
          fr.friendly_name));

      // Checkout submodules, if required.
      //
      for (const package_manifest& sm: pms)
      {
        dir_path d (td / path_cast<dir_path> (*sm.location));

        if (!exists (d) || empty (d))
        {
          git_checkout_submodules (co, rl, td);
          break;
        }
      }

      // Parse package manifests.
      //
      fr.packages = parse_package_manifests (co,
                                             td,
                                             move (pms),
                                             ignore_unknown,
                                             rl,
                                             fr.friendly_name);
      r.fragments.push_back (move (fr));
    }

    // Move the state directory to its proper place.
    //
    // If there is no configuration directory then we let auto_rmdir clean it
    // up from the the temporary directory.
    //
    if (!rd.empty ())
    {
      mv (td, rd);
      rm.cancel ();
      filesystem_state_changed = true;
    }

    return r;
  }

  rep_fetch_data
  rep_fetch (const common_options& co,
             const dir_path* conf,
             const repository_location& rl,
             bool iu)
  {
    switch (rl.type ())
    {
    case repository_type::pkg: return rep_fetch_pkg (co, conf, rl, iu);
    case repository_type::dir: return rep_fetch_dir (co, rl, iu);
    case repository_type::git: return rep_fetch_git (co, conf, rl, iu);
    }

    assert (false); // Can't be here.
    return rep_fetch_data ();
  }

  // Return an existing repository fragment or create a new one. Update the
  // existing object unless it is immutable (see repository_fragment class
  // description for details). Don't fetch the complement and prerequisite
  // repositories.
  //
  static shared_ptr<repository_fragment>
  rep_fragment (const common_options& co,
                const dir_path& conf,
                transaction& t,
                const repository_location& rl,
                rep_fetch_data::fragment&& fr)
  {
    tracer trace ("rep_fragment");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    bool mut (fr.id.empty ()); // Is the fragment mutable?

    // Calculate the fragment location.
    //
    repository_location rfl;

    switch (rl.type ())
    {
    case repository_type::pkg:
    case repository_type::dir:
      {
        assert (mut);

        rfl = rl;
        break;
      }
    case repository_type::git:
      {
        assert (!mut);

        repository_url url (rl.url ());
        url.fragment = move (fr.id);

        rfl = repository_location (url, rl.type ());
        break;
      }
    }

    shared_ptr<repository_fragment> rf (
      db.find<repository_fragment> (rfl.canonical_name ()));

    // Return the existing repository fragment if it is immutable.
    //
    bool exists (rf != nullptr);

    if (exists)
    {
      // Note that the user could change the repository URL the fragment was
      // fetched from. In this case we need to sync the fragment location with
      // the repository location to make sure that we use a proper URL for the
      // fragment checkout. Not doing so we, for example, may try to fetch git
      // submodules from the URL that is not available anymore.
      //
      if (rfl.url () != rf->location.url ())
      {
        rf->location = move (rfl);
        db.update (rf);
      }

      if (!mut)
        return rf;
    }

    // Create or update the repository fragment.
    //
    if (exists)
    {
      assert (mut);

      rf->complements.clear ();
      rf->prerequisites.clear ();
    }
    else
      rf = make_shared<repository_fragment> (move (rfl));

    for (repository_manifest& rm: fr.repositories)
    {
      repository_role rr (rm.effective_role ());

      if (rr == repository_role::base)
        continue; // Entry for this repository.

      repository_location& l (rm.location);

      // If the location is relative, complete it using this repository
      // as a base.
      //
      if (l.relative ())
      {
        try
        {
          l = repository_location (l, rl);
        }
        catch (const invalid_argument& e)
        {
          fail << "invalid relative repository location '" << l
               << "': " << e <<
            info << "base repository location is " << rl;
        }
      }

      // Create the new repository if it is not in the database yet, otherwise
      // update its location if it is changed, unless the repository is a
      // top-level one (and so its location is authoritative). Such a change
      // may root into the top-level repository location change made by the
      // user.
      //
      shared_ptr<repository> r (db.find<repository> (l.canonical_name ()));

      if (r == nullptr)
      {
        r = make_shared<repository> (move (l));
        db.persist (r); // Enter into session, important if recursive.
      }
      else if (r->location.url () != l.url ())
      {
        shared_ptr<repository_fragment> root (
          db.load<repository_fragment> (""));

        repository_fragment::complements_type& ua (root->complements);

        if (ua.find (r) == ua.end ())
        {
          r->location = move (l);
          db.update (r);
        }
      }

      // @@ What if we have duplicates? Ideally, we would like to check
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
          l4 ([&]{trace << r->name << " complement of " << rf->name;});
          rf->complements.insert (lazy_shared_ptr<repository> (db, r));
          break;
        }
      case repository_role::prerequisite:
        {
          l4 ([&]{trace << r->name << " prerequisite of " << rf->name;});
          rf->prerequisites.insert (lazy_weak_ptr<repository> (db, r));
          break;
        }
      case repository_role::base:
        assert (false);
      }
    }

    // For dir and git repositories that have neither prerequisites nor
    // complements we use the root repository as the default complement.
    //
    // This supports the common use case where the user has a single-package
    // repository and doesn't want to bother with the repositories.manifest
    // file. This way their package will still pick up its dependencies from
    // the configuration, without regards from which repositories they came
    // from.
    //
    switch (rl.type ())
    {
    case repository_type::git:
    case repository_type::dir:
      {
        if (rf->complements.empty () && rf->prerequisites.empty ())
          rf->complements.insert (lazy_shared_ptr<repository> (db, string ()));

        break;
      }
    case repository_type::pkg:
      {
        // Pkg repository is a "strict" one, that requires all the
        // prerequisites and complements to be listed.
        //
        break;
      }
    }

    if (exists)
      db.update (rf);
    else
      db.persist (rf);

    // "Suspend" session while persisting packages to reduce memory
    // consumption.
    //
    session& s (session::current ());
    session::reset_current ();

    // Remove this repository fragment from locations of the available
    // packages it contains.
    //
    if (exists)
      rep_remove_package_locations (t, rf->name);

    for (package_manifest& pm: fr.packages)
    {
      // Fix-up the external package version iteration number.
      //
      if (rl.directory_based ())
      {
        // Note that we can't check if the external package of this upstream
        // version and revision is already available in the configuration
        // until we fetch all the repositories, as some of the available
        // packages are still due to be removed.
        //
        optional<version> v (
          package_iteration (
            co,
            conf,
            t,
            path_cast<dir_path> (rl.path () / *pm.location),
            pm.name,
            pm.version,
            false /* check_external */));

        if (v)
          pm.version = move (*v);
      }

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
        assert (!p->locations.empty ()); // Can't be transient.

        // Note that sha256sum may not present for some repository types.
        //
        if (pm.sha256sum)
        {
          if (!p->sha256sum)
            p->sha256sum = move (pm.sha256sum);
          else if (*pm.sha256sum != *p->sha256sum)
          {
            // All the previous repositories that have checksum for this
            // package have it the same (since they passed this test), so we
            // can pick any to show to the user.
            //
            const string& r1 (rl.canonical_name ());
            const string& r2 (p->locations[0].repository_fragment.object_id ());

            fail << "checksum mismatch for " << pm.name << " " << pm.version <<
              info << r1 << " has " << *pm.sha256sum <<
              info << r2 << " has " << *p->sha256sum <<
              info << "consider reporting this to the repository maintainers";
          }
        }
      }

      p->locations.push_back (
        package_location {lazy_shared_ptr<repository_fragment> (db, rf),
                          move (*pm.location)});

      if (persist)
        db.persist (p);
      else
        db.update (p);
    }

    session::current (s); // "Resume".

    return rf;
  }

  using repositories         = set<shared_ptr<repository>>;
  using repository_fragments = set<shared_ptr<repository_fragment>>;

  // If reason is absent, then don't print the "fetching ..." progress line.
  //
  static void
  rep_fetch (const common_options& co,
             const dir_path& conf,
             transaction& t,
             const shared_ptr<repository>& r,
             repositories& fetched_repositories,
             repositories& removed_repositories,
             repository_fragments& removed_fragments,
             bool shallow,
             const optional<string>& reason)
  {
    tracer trace ("rep_fetch(rep)");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    // Check that the repository is not fetched yet and register it as fetched
    // otherwise.
    //
    // Note that we can end up with a repository dependency cycle via
    // prerequisites. Thus we register the repository before recursing into its
    // dependencies.
    //
    if (!fetched_repositories.insert (r).second) // Is already fetched.
      return;

    const repository_location& rl (r->location);
    l4 ([&]{trace << r->name << " " << rl;});

    // Cancel the repository removal.
    //
    // Note that this is an optimization as the rep_remove() function checks
    // for reachability of the repository being removed.
    //
    removed_repositories.erase (r);

    // The fetch_*() functions below will be quiet at level 1, which can be
    // quite confusing if the download hangs.
    //
    if (verb && reason)
    {
      diag_record dr (text);

      dr << "fetching " << r->name;

      if (!reason->empty ())
        dr << " (" << *reason << ")";
    }

    // Save the current complements and prerequisites to later check if the
    // shallow repository fetch is possible and to register them for removal
    // if that's not the case.
    //
    repository_fragment::complements_type old_complements;
    repository_fragment::prerequisites_type old_prerequisites;

    auto collect_deps = [] (const shared_ptr<repository_fragment>& rf,
                            repository_fragment::complements_type& cs,
                            repository_fragment::prerequisites_type& ps)
    {
      for (const auto& cr: rf->complements)
        cs.insert (cr);

      for (const auto& pr: rf->prerequisites)
        ps.insert (pr);
    };

    // While traversing fragments also register them for removal.
    //
    for (const repository::fragment_type& fr: r->fragments)
    {
      shared_ptr<repository_fragment> rf (fr.fragment.load ());

      collect_deps (rf, old_complements, old_prerequisites);
      removed_fragments.insert (rf);
    }

    // Cleanup the repository fragments list.
    //
    r->fragments.clear ();

    // Load the repository and package manifests and use them to populate the
    // repository fragments list, as well as its prerequisite and complement
    // repository sets.
    //
    rep_fetch_data rfd (rep_fetch (co, &conf, rl, true /* ignore_unknow */));

    repository_fragment::complements_type new_complements;
    repository_fragment::prerequisites_type new_prerequisites;

    for (rep_fetch_data::fragment& fr: rfd.fragments)
    {
      string nm (fr.friendly_name); // Don't move, still may be used.

      shared_ptr<repository_fragment> rf (
        rep_fragment (co, conf, t, rl, move (fr)));

      collect_deps (rf, new_complements, new_prerequisites);

      // Cancel the repository fragment removal.
      //
      // Note that this is an optimization as the rep_remove_fragment()
      // function checks if the fragment is dangling prio to the removal.
      //
      removed_fragments.erase (rf);

      r->fragments.push_back (
        repository::fragment_type {
          move (nm), lazy_shared_ptr<repository_fragment> (db, move (rf))});
    }

    // Save the changes to the repository object.
    //
    db.update (r);

    // Reset the shallow flag if the set of complements and/or prerequisites
    // has changed.
    //
    // Note that weak pointers are generally incomparable (as can point to
    // expired objects), and thus we can't compare the prerequisite sets
    // directly.
    //
    if (shallow)
      shallow = new_complements == old_complements &&
        equal (new_prerequisites.begin (), new_prerequisites.end (),
               old_prerequisites.begin (), old_prerequisites.end (),
               [] (const lazy_weak_ptr<repository>& x,
                   const lazy_weak_ptr<repository>& y)
               {
                 return x.object_id () == y.object_id ();
               });

    // Fetch prerequisites and complements, unless this is a shallow fetch.
    //
    if (!shallow)
    {
      // Register old complements and prerequisites for potential removal
      // unless they are fetched.
      //
      auto rm = [&fetched_repositories, &removed_repositories] (
        const lazy_shared_ptr<repository>& rp)
      {
        shared_ptr<repository> r (rp.load ());
        if (fetched_repositories.find (r) == fetched_repositories.end ())
          removed_repositories.insert (move (r));
      };

      for (const lazy_shared_ptr<repository>& cr: old_complements)
      {
        // Remove the complement unless it is the root repository (see
        // rep_fetch() for details).
        //
        if (cr.object_id () != "")
          rm (cr);
      }

      for (const lazy_weak_ptr<repository>& pr: old_prerequisites)
        rm (lazy_shared_ptr<repository> (pr));

      const string& rn (rl.canonical_name ());

      // Fetch complements and prerequisites.
      //
      for (const auto& cr: new_complements)
      {
        if (cr.object_id () != "")
          rep_fetch (co,
                     conf,
                     t,
                     cr.load (),
                     fetched_repositories,
                     removed_repositories,
                     removed_fragments,
                     false /* shallow */,
                     "complements " + rn);
      }

      for (const auto& pr: new_prerequisites)
        rep_fetch (co,
                   conf,
                   t,
                   pr.load (),
                   fetched_repositories,
                   removed_repositories,
                   removed_fragments,
                   false /* shallow */,
                   "prerequisite of " + rn);
    }
  }

  static void
  rep_fetch (const common_options& o,
             const dir_path& conf,
             transaction& t,
             const vector<lazy_shared_ptr<repository>>& repos,
             bool shallow,
             const optional<string>& reason)
  {
    tracer trace ("rep_fetch(repos)");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    // As a fist step we fetch repositories recursively building the list of
    // the former repository prerequisites, complements and fragments to be
    // considered for removal.
    //
    // We delay the actual removal until we fetch all the required repositories
    // as a dependency dropped by one repository can appear for another one.
    // The same is true about repository fragments.
    //
    try
    {
      // If fetch fails and the repository filesystem state is changed, then
      // the configuration is broken, and we have to take some drastic
      // measures (see below).
      //
      filesystem_state_changed = false;

      repositories         fetched_repositories;
      repositories         removed_repositories;
      repository_fragments removed_fragments;

      // Fetch the requested repositories, recursively.
      //
      for (const lazy_shared_ptr<repository>& r: repos)
        rep_fetch (o,
                   conf,
                   t,
                   r.load (),
                   fetched_repositories,
                   removed_repositories,
                   removed_fragments,
                   shallow,
                   reason);

      // Remove dangling repositories.
      //
      for (const shared_ptr<repository>& r: removed_repositories)
        rep_remove (conf, t, r);

      // Remove dangling repository fragments.
      //
      for (const shared_ptr<repository_fragment>& rf: removed_fragments)
        rep_remove_fragment (conf, t, rf);

      // Finally, make sure that the external packages are available from a
      // single directory-based repository.
      //
      // Sort the packages by name and version. This way the external packages
      // with the same upstream version and revision will be adjacent. Note
      // that here we rely on the fact that the directory-based repositories
      // have a single fragment.
      //
      using query = query<package_repository_fragment>;
      const auto& qv (query::package::id.version);

      query q ("ORDER BY" + query::package::id.name + "," +
               qv.epoch + "," +
               qv.canonical_upstream + "," +
               qv.canonical_release + "," +
               qv.revision + "," +
               qv.iteration);

      available_package_id ap;
      shared_ptr<repository_fragment> rf;

      for (const auto& prf: db.query<package_repository_fragment> (q))
      {
        const shared_ptr<repository_fragment>& f (prf.repository_fragment);
        if (!f->location.directory_based ())
          continue;

        // Fail if the external package is of the same upstream version and
        // revision as the previous one.
        //
        const available_package_id& id (prf.package_id);

        if (id.name == ap.name &&
            compare_version_eq (id.version, ap.version, true, false))
        {
          shared_ptr<available_package> p (db.load<available_package> (id));
          const version& v (p->version);

          fail << "external package " << id.name << '/'
               << version (v.epoch, v.upstream, v.release, v.revision, 0)
               << " is available from two repositories" <<
            info << "repository " << rf->location <<
            info << "repository " << f->location;
        }

        ap = id;
        rf = f;
      }
    }
    catch (const failed&)
    {
      t.rollback ();

      if (filesystem_state_changed)
      {
        // Warn prior to the cleanup operation that potentially can also fail.
        // Note that we assume that the diagnostics has already been issued.
        //
        warn << "repository state is now broken and will be cleaned up" <<
          info << "run 'bpkg rep-fetch' to update";

        rep_remove_clean (o, conf, t.database ());
      }

      throw;
    }
  }

  void
  rep_fetch (const common_options& o,
             const dir_path& conf,
             database& db,
             const vector<repository_location>& rls,
             bool shallow,
             const optional<string>& reason)
  {
    assert (session::has_current ());

    vector<lazy_shared_ptr<repository>> repos;
    repos.reserve (rls.size ());

    transaction t (db);

    shared_ptr<repository_fragment> root (db.load<repository_fragment> (""));

    // User-added repos.
    //
    repository_fragment::complements_type& ua (root->complements);

    for (const repository_location& rl: rls)
    {
      lazy_shared_ptr<repository> r (db, rl.canonical_name ());

      // Add the repository, unless it is already a top-level one and has the
      // same location.
      //
      if (ua.find (r) == ua.end () || r.load ()->location.url () != rl.url ())
        rep_add (o, t, rl);

      repos.emplace_back (r);
    }

    rep_fetch (o, conf, t, repos, shallow, reason);

    t.commit ();
  }

  int
  rep_fetch (const rep_fetch_options& o, cli::scanner& args)
  {
    tracer trace ("rep_fetch");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    // Build the list of repositories the user wants to fetch.
    //
    vector<lazy_shared_ptr<repository>> repos;

    database db (open (c, trace));
    transaction t (db);
    session s; // Repository dependencies can have cycles.

    shared_ptr<repository_fragment> root (db.load<repository_fragment> (""));

    // User-added repos.
    //
    repository_fragment::complements_type& ua (root->complements);

    optional<string> reason;

    if (!args.more ())
    {
      if (ua.empty ())
        fail << "configuration " << c << " has no repositories" <<
          info << "use 'bpkg rep-add' to add a repository";

      for (const lazy_shared_ptr<repository>& r: ua)
        repos.push_back (r);

      // Always print "fetching ..." for complements of the root, even if
      // there is only one.
      //
      reason = "";
    }
    else
    {
      while (args.more ())
      {
        // Try to map the argument to a user-added repository.
        //
        // If this is a repository name then it must be present in the
        // configuration. If this is a repository location then we add it to
        // the configuration.
        //
        lazy_shared_ptr<repository> r;
        string a (args.next ());

        if (repository_name (a))
        {
          r = lazy_shared_ptr<repository> (db, a);

          if (ua.find (r) == ua.end ())
            fail << "repository '" << a << "' does not exist in this "
                 << "configuration";
        }
        else
        {
          repository_location rl (parse_location (a, nullopt /* type */));
          r = lazy_shared_ptr<repository> (db, rl.canonical_name ());

          // If the repository is not the root complement yet or has
          // a different location then we add it to the configuration.
          //
          auto i (ua.find (r));
          if (i == ua.end () || i->load ()->location.url () != rl.url ())
            r = lazy_shared_ptr<repository> (db, rep_add (o, t, rl));
        }

        repos.emplace_back (move (r));
      }

      // If the user specified a single repository, then don't insult them
      // with a pointless "fetching ..." line for this repository.
      //
      if (repos.size () > 1)
      {
        // Also, as a special case (or hack, if you will), suppress these
        // lines if all the repositories are directory-based. For such
        // repositories there will never be any fetch progress nor can
        // they hang.
        //
        for (lazy_shared_ptr<repository> r: repos)
        {
          if (!r.load ()->location.directory_based ())
          {
            reason = "";
            break;
          }
        }
      }
    }

    rep_fetch (o, c, t, repos, o.shallow (), reason);

    size_t rcount (0), pcount (0);
    if (verb)
    {
      rcount = db.query_value<repository_count> ();
      pcount = db.query_value<available_package_count> ();
    }

    t.commit ();

    if (verb && !o.no_result ())
      text << pcount << " package(s) in " << rcount << " repository(s)";

    return 0;
  }
}
