// file      : bpkg/rep-fetch.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-fetch.hxx>

#include <set>

#include <libbutl/process.mxx>
#include <libbutl/process-io.mxx>      // operator<<(ostream, process_path)
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

    pkg_repository_manifests& rms (rmc.first);

    bool a (co.auth () != auth::none &&
            (co.auth () == auth::all || rl.remote ()));

    shared_ptr<const certificate> cert;
    const optional<string>& cert_pem (rms.back ().certificate);

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

    vector<rep_fetch_data::package> fps;
    fps.reserve (pms.size ());

    for (package_manifest& m: pms)
      fps.emplace_back (
        rep_fetch_data::package {move (m),
                                 string () /* repository_state */});

    return rep_fetch_data {move (rms), move (fps), move (cert)};
  }

  template <typename M>
  static M
  parse_manifest (const path& f, bool iu, const repository_location& rl)
  {
    try
    {
      ifdstream ifs (f);
      manifest_parser mp (ifs, f.string ());
      return M (mp, iu);
    }
    catch (const manifest_parsing& e)
    {
      fail (e.name, e.line, e.column) << e.description <<
        info << "repository " << rl << endf;
    }
    catch (const io_error& e)
    {
      fail << "unable to read from " << f << ": " << e <<
        info << "repository " << rl << endf;
    }
  }

  static rep_fetch_data
  rep_fetch_git (const common_options& co,
                 const dir_path* conf,
                 const repository_location& rl,
                 bool ignore_unknown)
  {
    // Plan:
    //
    // 1. Check repos_dir/<hash>/:
    //
    // 1.a If does not exist, git-clone into temp_dir/<hash>/<fragment>/.
    //
    // 1.a Otherwise, move as temp_dir/<hash>/ and git-fetch.
    //
    // 2. Move from temp_dir/<hash>/ to repos_dir/<hash>/<fragment>/
    //
    // 3. Check if repos_dir/<hash>/<fragment>/repositories exists:
    //
    // 3.a If exists, load.
    //
    // 3.b Otherwise, synthesize repository list with base repository.
    //
    // 4. Check if repos_dir/<hash>/<fragment>/packages exists:
    //
    // 4.a If exists, load. (into "skeleton" packages list to be filled?)
    //
    // 4.b Otherwise, synthesize as if single 'location: ./'.
    //
    // 5. For each package location obtained on step 4:
    //
    // 5.a Load repos_dir/<hash>/<fragment>/<location>/manifest.
    //
    // 5.b Run 'b info: repos_dir/<hash>/<fragment>/<location>/' and fix-up
    //     package version.
    //
    // 6. Return repository and package manifests (certificate is NULL).
    //

    if (conf != nullptr && conf->empty ())
      conf = dir_exists (bpkg_dir) ? &current_dir : nullptr;

    assert (conf == nullptr || !conf->empty ());

    // Clone or fetch the repository.
    //
    dir_path sd (repository_state (rl));

    auto_rmdir rm (temp_dir / sd);
    dir_path& td (rm.path);

    if (exists (td))
      rm_r (td);

    // If the git repository directory already exists, then we are fetching
    // an already cloned repository. Move it to the temporary directory.
    //
    // In this case also set the filesystem_state_changed flag since we are
    // modifying the repository filesystem state.
    //
    // In the future we can probably do something smarter about the flag,
    // keeping it unset unless the repository state directory is really
    // changed.
    //
    dir_path rd;
    bool fetch (false);
    if (conf != nullptr)
    {
      rd = *conf / repos_dir / sd;

      if (exists (rd))
      {
        mv (rd, td);
        filesystem_state_changed = true;
        fetch = true;
      }
    }

    dir_path nm (fetch ? git_fetch (co, rl, td) : git_clone (co, rl, td));
    dir_path fd (td / nm); // Full directory path.

    // Produce repository manifest list.
    //
    git_repository_manifests rms;
    {
      path f (fd / path ("repositories"));

      if (exists (f))
        rms = parse_manifest<git_repository_manifests> (f, ignore_unknown, rl);
      else
        rms.emplace_back (repository_manifest ()); // Add the base repository.
    }

    // Produce the "skeleton" package manifest list.
    //
    git_package_manifests pms;
    {
      path f (fd / path ("packages"));

      if (exists (f))
        pms = parse_manifest<git_package_manifests> (f, ignore_unknown, rl);
      else
      {
        pms.push_back (package_manifest ());
        pms.back ().location = current_dir;
      }
    }

    vector<rep_fetch_data::package> fps;
    fps.reserve (pms.size ());

    // Parse package manifests.
    //
    for (package_manifest& sm: pms)
    {
      assert (sm.location);

      auto package_info = [&sm, &rl] (diag_record& dr)
      {
        dr << "package ";

        if (!sm.location->current ())
          dr << "'" << sm.location->string () << "' "; // Strip trailing '/'.

        dr << "in repository " << rl;
      };

      auto failure = [&package_info] (const char* desc)
      {
        diag_record dr (fail);
        dr << desc << " for ";
        package_info (dr);
      };

      dir_path d (fd / path_cast<dir_path> (*sm.location));
      path f (d / path ("manifest"));

      if (!exists (f))
        failure ("no manifest file");

      try
      {
        ifdstream ifs (f);
        manifest_parser mp (ifs, f.string ());
        package_manifest m (pkg_package_manifest (mp, ignore_unknown));

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
        diag_record dr (fail);
        dr << "unable to read from " << f << ": " << e << info;
        package_info (dr);
      }

      // Fix-up the package version.
      //
      const char* b (name_b (co));

      try
      {
        process_path pp (process::path_search (b, exec_dir));

        fdpipe pipe (open_pipe ());

        process pr (
          process_start_callback (
            [] (const char* const args[], size_t n)
            {
              if (verb >= 2)
                print_process (args, n);
            },
            0 /* stdin */, pipe /* stdout */, 2 /* stderr */,
            pp,

            verb < 2
            ? strings ({"-q"})
            : verb == 2
              ? strings ({"-v"})
              : strings ({"--verbose", to_string (verb)}),

            co.build_option (),
            "info:",
            d.representation ()));

        // Shouldn't throw, unless something is severely damaged.
        //
        pipe.out.close ();

        try
        {
          ifdstream is (move (pipe.in),
                        fdstream_mode::skip,
                        ifdstream::badbit);

          for (string l; !eof (getline (is, l)); )
          {
            if (l.compare (0, 9, "version: ") == 0)
            try
            {
              string v (l, 9);

              // An empty version indicates that the version module is not
              // enabled for the project, and so we don't amend the package
              // version.
              //
              if (!v.empty ())
                sm.version = version (v);

              break;
            }
            catch (const invalid_argument&)
            {
              fail << "no package version in '" << l << "'" <<
                info << "produced by '" << pp << "'; use --build to override";
            }
          }

          is.close ();

          // If succeess then save the package manifest together with the
          // repository state it belongs to and go to the next package.
          //
          if (pr.wait ())
          {
            fps.emplace_back (rep_fetch_data::package {move (sm),
                                                       nm.string ()});
            continue;
          }

          // Fall through.
        }
        catch (const io_error&)
        {
          if (pr.wait ())
            failure ("unable to read information");

          // Fall through.
        }

        // We should only get here if the child exited with an error status.
        //
        assert (!pr.wait ());

        failure ("unable to obtain information");
      }
      catch (const process_error& e)
      {
        fail << "unable to execute " << b << ": " << e;
      }
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

    return rep_fetch_data {move (rms), move (fps), nullptr};
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
    case repository_type::git: return rep_fetch_git (co, conf, rl, iu);
    }

    assert (false); // Can't be here.
    return rep_fetch_data ();
  }

  using repositories = set<shared_ptr<repository>>;

  static void
  rep_fetch (const common_options& co,
             const dir_path& conf,
             database& db,
             const shared_ptr<repository>& r,
             repositories& fetched,
             repositories& removed,
             const string& reason = string ())
  {
    tracer trace ("rep_fetch(rep)");

    tracer_guard tg (db, trace);

    // Check that the repository is not fetched yet and register it as fetched
    // otherwise.
    //
    // Note that we can end up with a repository dependency cycle via
    // prerequisites. Thus we register the repository before recursing into its
    // dependencies.
    //
    if (!fetched.insert (r).second) // Is already fetched.
      return;

    const repository_location& rl (r->location);
    l4 ([&]{trace << r->name << " " << rl;});

    // Cancel the repository removal.
    //
    // Note that this is an optimization as the rep_remove() function checks
    // for reachability of the repository being removed.
    //
    removed.erase (r);

    // The fetch_*() functions below will be quiet at level 1, which
    // can be quite confusing if the download hangs.
    //
    if (verb)
    {
      diag_record dr (text);

      dr << "fetching " << r->name;

      if (!reason.empty ())
        dr << " (" << reason << ")";
    }

    // Register complements and prerequisites for potential removal unless
    // they are fetched. Clear repository dependency sets afterwards.
    //
    auto remove = [&fetched, &removed] (const lazy_shared_ptr<repository>& rp)
    {
      shared_ptr<repository> r (rp.load ());
      if (fetched.find (r) == fetched.end ())
        removed.insert (move (r));
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

    r->complements.clear ();
    r->prerequisites.clear ();

    // Remove this repository from locations of the available packages it
    // contains.
    //
    rep_remove_package_locations (db, r->name);

    // Load the repository and package manifests and use them to populate the
    // prerequisite and complement repository sets as well as available
    // packages.
    //
    rep_fetch_data rfd (rep_fetch (co, &conf, rl, true /* ignore_unknow */));

    for (repository_manifest& rm: rfd.repositories)
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

      // Create the new repository if it is not in the database yet. Otherwise
      // update its location.
      //
      shared_ptr<repository> pr (db.find<repository> (l.canonical_name ()));

      if (pr == nullptr)
      {
        pr = make_shared<repository> (move (l));
        db.persist (pr); // Enter into session, important if recursive.
      }
      else if (pr->location.url () != l.url ())
      {
        pr->location = move (l);
        db.update (r);
      }

      // Load the prerequisite repository.
      //
      string reason;
      switch (rr)
      {
      case repository_role::complement:   reason = "complements ";     break;
      case repository_role::prerequisite: reason = "prerequisite of "; break;
      case repository_role::base: assert (false);
      }
      reason += r->name;

      rep_fetch (co, conf, db, pr, fetched, removed, reason);

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

    // For git repositories that have neither prerequisites nor complements
    // we use the root repository as the default complement.
    //
    // This supports the common use case where the user has a single-package
    // git repository and doesn't want to bother with the repositories file.
    // This way their package will still pick up its dependencies from the
    // configuration, without regards from which repositories they came from.
    //
    if (rl.type () == repository_type::git &&
        r->complements.empty ()            &&
        r->prerequisites.empty ())
      r->complements.insert (lazy_shared_ptr<repository> (db, string ()));

    // Save the changes to the repository object.
    //
    db.update (r);

    // "Suspend" session while persisting packages to reduce memory
    // consumption.
    //
    session& s (session::current ());
    session::reset_current ();

    for (rep_fetch_data::package& fp: rfd.packages)
    {
      package_manifest& pm (fp.manifest);

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
            const string& r2 (p->locations[0].repository.object_id ());

            fail << "checksum mismatch for " << pm.name << " " << pm.version <<
              info << r1 << " has " << *pm.sha256sum <<
              info << r2 << " has " << *p->sha256sum <<
              info << "consider reporting this to the repository maintainers";
          }
        }
      }

      // This repository shouldn't already be in the location set since
      // that would mean it has already been loaded and we shouldn't be
      // here.
      //
      p->locations.push_back (
        package_location {lazy_shared_ptr<repository> (db, r),
                          move (fp.repository_fragment),
                          move (*pm.location)});

      if (persist)
        db.persist (p);
      else
        db.update (p);
    }

    session::current (s); // "Resume".
  }

  static void
  rep_fetch (const common_options& o,
             const dir_path& conf,
             transaction& t,
             const vector<lazy_shared_ptr<repository>>& repos)
  {
    database& db (t.database ());

    // As a fist step we fetch repositories recursively building the list of
    // the former prerequisites and complements to be considered for removal.
    //
    // We delay the actual removal until we fetch all the required repositories
    // as a dependency dropped by one repository can appear for another one.
    //
    try
    {
      // If fetch fails and the repository filesystem state is changed, then
      // the configuration is broken, and we have to take some drastic
      // measures (see below).
      //
      filesystem_state_changed = false;

      repositories fetched;
      repositories removed;

      for (const lazy_shared_ptr<repository>& r: repos)
        rep_fetch (o, conf, db, r.load (), fetched, removed);

      // Finally, remove dangling repositories.
      //
      for (const shared_ptr<repository>& r: removed)
        rep_remove (conf, db, r);
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

        rep_remove_clean (conf, db);
      }

      throw;
    }
  }

  void
  rep_fetch (const common_options& o,
             const dir_path& conf,
             database& db,
             const vector<repository_location>& rls)
  {
    vector<lazy_shared_ptr<repository>> repos;
    repos.reserve (rls.size ());

    transaction t (db.begin ());

    shared_ptr<repository> root (db.load<repository> (""));
    repository::complements_type& ua (root->complements); // User-added repos.

    for (const repository_location& rl: rls)
    {
      lazy_shared_ptr<repository> r (db, rl.canonical_name ());

      // Add the repository, unless it is already a top-level one and has the
      // same location.
      //
      if (ua.find (r) == ua.end () || r.load ()->location.url () != rl.url ())
        rep_add (db, rl);

      repos.emplace_back (r);
    }

    rep_fetch (o, conf, t, repos);

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
    transaction t (db.begin ());
    session s; // Repository dependencies can have cycles.

    shared_ptr<repository> root (db.load<repository> (""));
    repository::complements_type& ua (root->complements); // User-added repos.

    if (!args.more ())
    {
      if (ua.empty ())
        fail << "configuration " << c << " has no repositories" <<
          info << "use 'bpkg rep-add' to add a repository";

      for (const lazy_shared_ptr<repository>& r: ua)
        repos.push_back (r);
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
          lazy_shared_ptr<repository> rp (db, a);

          if (ua.find (rp) != ua.end ())
            r = move (rp);
          else
            fail << "repository '" << a << "' does not exist in this "
                 << "configuration";
        }
        else
          //@@ TODO: check if exists in root & same location and avoid
          // calling rep_add. Get rid of quiet mode.
          //
          r = lazy_shared_ptr<repository> (
            db, rep_add (db, parse_location (a, nullopt /* type */)));

        repos.emplace_back (move (r));
      }
    }

    rep_fetch (o, c, t, repos);

    size_t rcount (0), pcount (0);
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
