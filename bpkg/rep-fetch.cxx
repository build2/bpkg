// file      : bpkg/rep-fetch.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-fetch.hxx>

#include <libbutl/sha256.mxx>
#include <libbutl/process.mxx>
#include <libbutl/process-io.mxx>      // operator<<(ostream, process_path)
#include <libbutl/manifest-parser.mxx>

#include <bpkg/auth.hxx>
#include <bpkg/fetch.hxx>
#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  static rep_fetch_data
  rep_fetch_bpkg (const common_options& co,
                  const dir_path* conf,
                  const repository_location& rl,
                  bool ignore_unknown)
  {
    // First fetch the repositories list and authenticate the base's
    // certificate.
    //
    pair<bpkg_repository_manifests, string /* checksum */> rmc (
      bpkg_fetch_repositories (co, rl, ignore_unknown));

    bpkg_repository_manifests& rms (rmc.first);

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
    pair<bpkg_package_manifests, string /* checksum */> pmc (
      bpkg_fetch_packages (co, rl, ignore_unknown));

    bpkg_package_manifests& pms (pmc.first);

    if (rmc.second != pms.sha256sum)
      fail << "repositories manifest file checksum mismatch for "
           << rl.canonical_name () <<
        info << "try again";

    if (a)
    {
      signature_manifest sm (
        bpkg_fetch_signature (co, rl, true /* ignore_unknown */));

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
    // If changing the repository directory naming scheme, then don't forget
    // to also update pkg_checkout().
    //
    dir_path h (sha256 (rl.canonical_name ()).abbreviated_string (16));

    auto_rmdir rm (temp_dir / h);
    dir_path& td (rm.path);

    if (exists (td))
      rm_r (td);

    // If the git repository directory already exists, then we are fetching
    // an already cloned repository. Move it to the temporary directory.
    //
    dir_path rd;
    bool fetch (false);
    if (conf != nullptr)
    {
      rd = *conf / repos_dir / h;

      if (exists (rd))
      {
        mv (rd, td);
        fetch = true;
      }
    }

    dir_path nm (fetch ? git_fetch (co, rl, td) : git_clone (co, rl, td));

    if (!rd.empty ())
      mv (td, rd);
    else
      // If there is no configuration directory then we leave the repository
      // in the temporary directory.
      //
      rd = move (td);

    rm.cancel ();

    rd /= nm;

    // Produce repository manifest list.
    //
    git_repository_manifests rms;
    {
      path f (rd / path ("repositories"));

      if (exists (f))
        rms = parse_manifest<git_repository_manifests> (f, ignore_unknown, rl);
      else
        rms.emplace_back (repository_manifest ()); // Add the base repository.
    }

    // Produce the "skeleton" package manifest list.
    //
    git_package_manifests pms;
    {
      path f (rd / path ("packages"));

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

      dir_path d (rd / path_cast<dir_path> (*sm.location));
      path f (d / path ("manifest"));

      if (!exists (f))
        failure ("no manifest file");

      try
      {
        ifdstream ifs (f);
        manifest_parser mp (ifs, f.string ());
        package_manifest m (bpkg_package_manifest (mp, ignore_unknown));

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
    case repository_type::bpkg: return rep_fetch_bpkg (co, conf, rl, iu);
    case repository_type::git:  return rep_fetch_git  (co, conf, rl, iu);
    }

    assert (false); // Can't be here.
    return rep_fetch_data ();
  }

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

    // Load the repositories and packages and use it to populate the
    // prerequisite and complement repository sets as well as available
    // packages.
    //
    rep_fetch_data rfd (
      rep_fetch (co, &co.directory (), rl, true /* ignore_unknow */));

    for (repository_manifest& rm: rfd.repositories)
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
      r->complements.insert (lazy_shared_ptr<repository> (db, root));

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
