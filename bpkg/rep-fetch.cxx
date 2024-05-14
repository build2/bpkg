// file      : bpkg/rep-fetch.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-fetch.hxx>

#include <map>
#include <set>

#include <libbutl/manifest-parser.hxx>

#include <bpkg/auth.hxx>
#include <bpkg/fetch.hxx>
#include <bpkg/rep-add.hxx>
#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/rep-remove.hxx>
#include <bpkg/pkg-verify.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/satisfaction.hxx>
#include <bpkg/package-query.hxx>
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

  inline static bool
  need_auth (const common_options& co, const repository_location& rl)
  {
    return rl.type () == repository_type::pkg && co.auth () != auth::none &&
      (co.auth () == auth::all || rl.remote ());
  }

  static rep_fetch_data
  rep_fetch_pkg (const common_options& co,
                 const dir_path* conf,
                 database* db,
                 const repository_location& rl,
                 const optional<string>& dependent_trust,
                 bool ignore_unknown,
                 bool ignore_toolchain)
  {
    // First fetch the repositories list and authenticate the base's
    // certificate.
    //
    pair<pkg_repository_manifests, string /* checksum */> rmc (
      pkg_fetch_repositories (co, rl, ignore_unknown));

    rep_fetch_data::fragment fr;
    fr.repositories = move (rmc.first);

    bool a (need_auth (co, rl));

    shared_ptr<const certificate> cert;
    optional<string> cert_pem (
      find_base_repository (fr.repositories).certificate);

    if (a)
    {
      cert = authenticate_certificate (
        co, conf, db, cert_pem, rl, dependent_trust);

      a = !cert->dummy ();
    }

    // Now fetch the packages list and make sure it matches the repositories
    // we just fetched.
    //
    pair<pkg_package_manifests, string /* checksum */> pmc (
      pkg_fetch_packages (co, rl, ignore_unknown));

    pkg_package_manifests& pms (pmc.first);

    if (rmc.second != pms.sha256sum)
    {
      error << "repositories manifest file checksum mismatch for "
            << rl.canonical_name () <<
        info << "try again";

      throw recoverable ();
    }

    fr.packages = move (pms);

    if (a)
    {
      signature_manifest sm (
        pkg_fetch_signature (co, rl, true /* ignore_unknown */));

      if (sm.sha256sum != pmc.second)
      {
        error << "packages manifest file checksum mismatch for "
              << rl.canonical_name () <<
          info << "try again";

        throw recoverable ();
      }

      assert (cert != nullptr);
      authenticate_repository (co, conf, cert_pem, *cert, sm, rl);
    }

    // If requested, verify that the packages are compatible with the current
    // toolchain.
    //
    if (!ignore_toolchain)
    {
      for (const package_manifest& m: fr.packages)
      {
        for (const dependency_alternatives& das: m.dependencies)
          toolchain_buildtime_dependency (co, das, &m.name);
      }
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

    if (r.empty ())
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

  static void
  print_package_info (diag_record& dr,
                      const dir_path& pl,
                      const repository_location& rl,
                      const optional<string>& fragment)
  {
    dr << "package ";

    if (!pl.current ())
      dr << "'" << pl.string () << "' "; // Strip trailing '/'.

    dr << "in repository " << rl;

    if (fragment)
      dr << ' ' << *fragment;
  }

  // Parse package manifests referenced by the package directory manifests.
  //
  static pair<vector<package_manifest>, vector<package_info>>
  parse_package_manifests (const common_options& co,
                           const dir_path& repo_dir,
                           vector<package_manifest>&& pms,
                           bool iu,
                           bool it,
                           const repository_location& rl,
                           const optional<string>& fragment) // For diagnostics.
  {
    auto prn_package_info = [&rl, &fragment] (diag_record& dr,
                                              const package_manifest& pm)
    {
      print_package_info (dr,
                          path_cast<dir_path> (*pm.location),
                          rl,
                          fragment);
    };

    // Verify that all the package directories contain the package manifest
    // files and retrieve the package versions via the single `b info` call,
    // but only if the current build2 version is satisfactory for all the
    // repository packages. While at it cache the manifest paths for the
    // future use.
    //
    // Note that if the package is not compatible with the toolchain, not to
    // end up with an unfriendly build2 error message (referring a line in the
    // bootstrap file issued by the version module), we need to verify the
    // compatibility of the package manifests prior to calling `b info`. Also
    // note that we cannot create the package manifest objects at this stage,
    // since we need the package versions for that. Thus, we cache the
    // respective name value lists instead.
    //
    optional<package_version_infos>     pvs;
    paths                               mfs;
    vector<vector<manifest_name_value>> nvs;
    {
      mfs.reserve (pms.size ());
      nvs.reserve (pms.size ());

      dir_paths pds;
      pds.reserve (pms.size ());

      // If true, then build2 version is satisfactory for all the repository
      // packages.
      //
      bool bs (true);

      for (const package_manifest& pm: pms)
      {
        assert (pm.location);

        dir_path d (repo_dir / path_cast<dir_path> (*pm.location));
        d.normalize (); // In case location is './'.

        path f (d / manifest_file);
        if (!exists (f))
        {
          diag_record dr (fail);
          dr << "no manifest file for ";
          prn_package_info (dr, pm);
        }

        // Provide the context if the package compatibility verification fails.
        //
        auto g (
          make_exception_guard (
            [&pm, &prn_package_info] ()
            {
              diag_record dr (info);

              dr << "while retrieving information for ";
              prn_package_info (dr, pm);
            }));

        try
        {
          ifdstream ifs (f);
          manifest_parser mp (ifs, f.string ());

          // Note that the package directory points to something temporary
          // (e.g., .bpkg/tmp/6f746365314d/) and it's probably better to omit
          // it entirely (the above exception guard will print all we've got).
          //
          pkg_verify_result r (pkg_verify (co, mp, it, dir_path ()));

          if (bs                  &&
              r.build2_dependency &&
              !satisfy_build2 (co, *r.build2_dependency))
          {
            bs = false;
            pds.clear (); // Won't now be used.
          }

          nvs.push_back (move (r));
        }
        catch (const manifest_parsing& e)
        {
          fail (e.name, e.line, e.column) << e.description;
        }
        catch (const io_error& e)
        {
          fail << "unable to read from " << f << ": " << e;
        }

        mfs.push_back (move (f));

        if (bs)
          pds.push_back (move (d));
      }

      // Note that for the directory-based repositories we also query
      // subprojects since the package information will be used for the
      // subsequent package_iteration() call (see below).
      //
      if (bs)
        pvs = package_versions (co, pds,
                                (rl.directory_based ()
                                 ? b_info_flags::subprojects
                                 : b_info_flags::none));
    }

    // Parse package manifests, fixing up their versions.
    //
    pair<vector<package_manifest>, vector<package_info>> r;
    r.first.reserve (pms.size ());

    if (pvs)
      r.second.reserve (pms.size ());

    for (size_t i (0); i != pms.size (); ++i)
    {
      package_manifest& pm (pms[i]);

      assert (pm.location);

      try
      {
        package_manifest m (
          mfs[i].string (),
          move (nvs[i]),
          [&pvs, i] (version& v)
          {
            if (pvs)
            {
              optional<version>& pv ((*pvs)[i].version);

              if (pv)
                v = move (*pv);
            }
          },
          iu);

        // Save the package manifest, preserving its location.
        //
        m.location = move (*pm.location);

        pm = move (m);
      }
      catch (const manifest_parsing& e)
      {
        diag_record dr (fail (e.name, e.line, e.column));
        dr << e.description << info;
        prn_package_info (dr, pm);
      }

      r.first.push_back  (move (pm));

      if (pvs)
        r.second.push_back (move ((*pvs)[i].info));
    }

    return r;
  }

  // Return contents of a file referenced by a *-file package manifest value.
  //
  static string
  read_package_file (const path& f,
                     const string& name,
                     const dir_path& pkg,
                     const dir_path& repo,
                     const repository_location& rl,
                     const string& fragment)
  {
    path rp (pkg / f);
    path fp (repo / rp);

    try
    {
      ifdstream is (fp);
      string s (is.read_text ());

      if (s.empty () && name != "build-file")
        fail << name << " manifest value in " << pkg / manifest_file
             << " references empty file " << rp <<
          info << "repository " << rl
               << (!fragment.empty () ? ' ' + fragment : "");

      return s;
    }
    catch (const io_error& e)
    {
      fail << "unable to read from " << rp << " referenced by "
           << name << " manifest value in " << pkg / manifest_file << ": "
           << e <<
        info << "repository " << rl
             << (!fragment.empty () ? ' ' + fragment : "")  << endf;
    }
  }

  static rep_fetch_data
  rep_fetch_dir (const common_options& co,
                 const repository_location& rl,
                 bool iu,
                 bool it,
                 bool ev,
                 bool lb)
  {
    assert (rl.absolute ());

    dir_path rd (path_cast<dir_path> (rl.path ()));

    rep_fetch_data::fragment fr;

    fr.repositories = parse_repository_manifests<dir_repository_manifests> (
      rd / repositories_file,
      iu,
      rl,
      string () /* fragment */);

    dir_package_manifests pms (
      parse_directory_manifests<dir_package_manifests> (
        rd / packages_file,
        iu,
        rl,
        string () /* fragment */));

    pair<vector<package_manifest>, vector<package_info>> pmi (
      parse_package_manifests (co,
                               rd,
                               move (pms),
                               iu,
                               it,
                               rl,
                               empty_string /* fragment */));

    fr.packages      = move (pmi.first);
    fr.package_infos = move (pmi.second);

    // If requested, expand file-referencing package manifest values and load
    // the buildfiles into the respective *-build values.
    //
    if (ev || lb)
    {
      for (package_manifest& m: fr.packages)
      {
        dir_path pl (path_cast<dir_path> (*m.location));

        // Load *-file values.
        //
        try
        {
          m.load_files (
            [ev, &rd, &rl, &pl]
            (const string& n, const path& p) -> optional<string>
            {
              // Always expand the build-file values.
              //
              if (ev || n == "build-file")
              {
                return read_package_file (p,
                                          n,
                                          pl,
                                          rd,
                                          rl,
                                          empty_string /* fragment */);
              }
              else
                return nullopt;
            },
            iu);
        }
        catch (const manifest_parsing& e)
        {
          diag_record dr (fail);
          dr << e << info;
          print_package_info (dr, pl, rl, nullopt /* fragment */);
          dr << endf;
        }

        // Load the bootstrap, root, and config/*.build buildfiles into the
        // respective *-build values, if requested and if they are not already
        // specified in the manifest.
        //
        if (lb)
        try
        {
          load_package_buildfiles (m, rd / pl, true /* err_path_relative */);
        }
        catch (const runtime_error& e)
        {
          diag_record dr (fail);
          dr << e << info;
          print_package_info (dr, pl, rl, nullopt /* fragment */);
          dr << endf;
        }
      }
    }

    return rep_fetch_data {{move (fr)},
                           nullopt /* cert_pem */,
                           nullptr /* certificate */};
  }

  static rep_fetch_data
  rep_fetch_git (const common_options& co,
                 const dir_path* conf,
                 const repository_location& rl,
                 bool iu,
                 bool it,
                 bool ev,
                 bool lb)
  {
    auto i (tmp_dirs.find (conf != nullptr ? *conf : empty_dir_path));
    assert (i != tmp_dirs.end ());

    dir_path sd (repository_state (rl));

    auto_rmdir rm (i->second / sd, !keep_tmp);
    const dir_path& td (rm.path);

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
    // - If packages.manifest file exists then load it into the "skeleton"
    //   packages list. Otherwise, synthesize it with the single:
    //
    //   location: ./
    //
    // - If any of the package locations point to non-existent directory, then
    //   assume it to be in a submodule and checkout submodules, recursively.
    //
    // - For each package location parse the package manifest.
    //
    // - Save the fragment identified by the commit id and containing the
    //   parsed repository and package manifest lists into the resulting
    //   fragment list.
    //
    rep_fetch_data r;
    size_t np (0);

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
          iu,
          rl,
          fr.friendly_name);

      // Parse package skeleton manifests.
      //
      git_package_manifests pms (
        parse_directory_manifests<git_package_manifests> (
          td / packages_file,
          iu,
          rl,
          fr.friendly_name));

      // Checkout submodules on the first call.
      //
      bool cs (true);
      auto checkout_submodules = [&co, &rl, &td, &cs] ()
      {
        if (cs)
        {
          git_checkout_submodules (co, rl, td);
          cs = false;
        }
      };

      // Checkout submodules to parse package manifests, if required.
      //
      for (const package_manifest& sm: pms)
      {
        dir_path d (td / path_cast<dir_path> (*sm.location));

        if (!exists (d) || empty (d))
        {
          checkout_submodules ();
          break;
        }
      }

      // Parse package manifests.
      //
      pair<vector<package_manifest>, vector<package_info>> pmi (
        parse_package_manifests (co,
                                 td,
                                 move (pms),
                                 iu,
                                 it,
                                 rl,
                                 fr.friendly_name));

      fr.packages      = move (pmi.first);
      fr.package_infos = move (pmi.second);

      // If requested, expand file-referencing package manifest values
      // checking out submodules, if required, and load the buildfiles into
      // the respective *-build values.
      //
      if (ev || lb)
      {
        for (package_manifest& m: fr.packages)
        {
          dir_path pl (path_cast<dir_path> (*m.location));

          // Load *-file values.
          //
          try
          {
            m.load_files (
              [ev, &td, &rl, &pl, &fr, &checkout_submodules]
              (const string& n, const path& p) -> optional<string>
              {
                // Always expand the build-file values.
                //
                if (ev || n == "build-file")
                {
                  // Check out submodules if the referenced file doesn't exist.
                  //
                  // Note that this doesn't work for symlinks on Windows where
                  // git normally creates filesystem-agnostic symlinks that
                  // are indistinguishable from regular files (see
                  // fixup_worktree() for details). It seems like the only way
                  // to deal with that is to unconditionally checkout
                  // submodules on Windows. Let's not pessimize things for now
                  // (if someone really wants this to work, they can always
                  // enable real symlinks in git).
                  //
                  if (!exists (td / pl / p))
                    checkout_submodules ();

                  return read_package_file (p,
                                            n,
                                            pl,
                                            td,
                                            rl,
                                            fr.friendly_name);
                }
                else
                  return nullopt;
              },
              iu);
          }
          catch (const manifest_parsing& e)
          {
            diag_record dr (fail);
            dr << e << info;
            print_package_info (dr, pl, rl, fr.friendly_name);
            dr << endf;
          }

          // Load the bootstrap, root, and config/*.build buildfiles into the
          // respective *-build values, if requested and if they are not
          // already specified in the manifest.
          //
          if (lb)
          try
          {
            load_package_buildfiles (m, td / pl, true /* err_path_relative */);
          }
          catch (const runtime_error& e)
          {
            diag_record dr (fail);
            dr << e << info;
            print_package_info (dr, pl, rl, fr.friendly_name);
            dr << endf;
          }
        }
      }

      np += fr.packages.size ();

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

    if (np == 0 && !rl.url ().fragment)
      warn << "repository " << rl << " has no available packages" <<
        info << "consider specifying explicit URL fragment (for example, "
             << "#master)";

    return r;
  }

  static rep_fetch_data
  rep_fetch (const common_options& co,
             const dir_path* conf,
             database* db,
             const repository_location& rl,
             const optional<string>& dt,
             bool iu,
             bool it,
             bool ev,
             bool lb)
  {
    switch (rl.type ())
    {
    case repository_type::pkg:
      {
        return rep_fetch_pkg (co, conf, db, rl, dt, iu, it);
      }
    case repository_type::dir:
      {
        return rep_fetch_dir (co, rl, iu, it, ev, lb);
      }
    case repository_type::git:
      {
        return rep_fetch_git (co, conf, rl, iu, it, ev, lb);
      }
    }

    assert (false); // Can't be here.
    return rep_fetch_data ();
  }

  rep_fetch_data
  rep_fetch (const common_options& co,
             const dir_path* conf,
             const repository_location& rl,
             bool iu,
             bool it,
             bool ev,
             bool lb)
  {
    return rep_fetch (co,
                      conf,
                      nullptr /* database */,
                      rl,
                      nullopt /* dependent_trust */,
                      iu,
                      it,
                      ev,
                      lb);
  }

  // Return an existing repository fragment or create a new one. Update the
  // existing object unless it is already fetched. Don't fetch the complement
  // and prerequisite repositories.
  //
  using repository_fragments = set<shared_ptr<repository_fragment>>;
  using repository_trust     = map<shared_ptr<repository>, optional<string>>;

  static shared_ptr<repository_fragment>
  rep_fragment (const common_options& co,
                database& db,
                transaction& t,
                const repository_location& rl,
                rep_fetch_data::fragment&& fr,
                repository_fragments& parsed_fragments,
                bool full_fetch,
                repository_trust& repo_trust)
  {
    tracer trace ("rep_fragment");

    tracer_guard tg (db, trace);

    // Calculate the fragment location.
    //
    repository_location rfl;

    switch (rl.type ())
    {
    case repository_type::pkg:
    case repository_type::dir:
      {
        rfl = rl;
        break;
      }
    case repository_type::git:
      {
        repository_url url (rl.url ());
        url.fragment = move (fr.id);

        rfl = repository_location (url, rl.type ());
        break;
      }
    }

    shared_ptr<repository_fragment> rf (
      db.find<repository_fragment> (rfl.canonical_name ()));

    // Complete the repository manifest relative locations using this
    // repository as a base.
    //
    for (repository_manifest& rm: fr.repositories)
    {
      if (rm.effective_role () != repository_role::base)
      {
        repository_location& l (rm.location);

        if (l.relative ())
        {
          try
          {
            l = repository_location (l, rl);
          }
          catch (const invalid_argument& e)
          {
            fail << "invalid relative repository location '" << l << "': "
                 << e <<
              info << "base repository location is " << rl;
          }

          // If the local prerequisite git repository having the .git extension
          // doesn't exist but the one without the extension does, then we
          // strip the extension from the location.
          //
          if (l.local ()                        &&
              l.type () == repository_type::git &&
              l.path ().extension () == "git")
          {
            dir_path d (path_cast<dir_path> (l.path ()));

            if (!exists (d) && exists (d.base () / dir_path (".git")))
            {
              repository_url u (l.url ());

              assert (u.path);
              u.path->make_base ();

              l = repository_location (u, l.type ());
            }
          }
        }
      }
    }

    // Add/upgrade (e.g., from the latest commit) the dependent trust for
    // the prerequisite/complement repositories. Here we rely on the fact that
    // rep_fragment() function is called for fragments (e.g., commits) in
    // earliest to latest order, as they appear in the repository object.
    //
    // Note that we also rely on the manifest location values be
    // absolute/remote (see above) and the corresponding repository objects be
    // present in the database.
    //
    auto add_trust = [&fr, &repo_trust, &db] ()
    {
      for (repository_manifest& rm: fr.repositories)
      {
        if (rm.effective_role () != repository_role::base)
        {
          auto i (repo_trust.emplace (
                    db.load<repository> (rm.location.canonical_name ()),
                    rm.trust));

          if (!i.second)
            i.first->second = move (rm.trust);
        }
      }
    };

    // Create or update the repository fragment.
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
    }
    else
      rf = make_shared<repository_fragment> (move (rfl));

    if (!parsed_fragments.insert (rf).second) // Is already parsed.
    {
      assert (exists);

      add_trust ();
      return rf;
    }

    rf->complements.clear ();
    rf->prerequisites.clear ();

    for (repository_manifest& rm: fr.repositories)
    {
      repository_role rr (rm.effective_role ());

      if (rr == repository_role::base)
        continue; // Entry for this repository.

      const repository_location& l (rm.location);

      // Create the new repository if it is not in the database yet, otherwise
      // update its location if it is changed, unless the repository is a
      // top-level one (and so its location is authoritative). Such a change
      // may root into the top-level repository location change made by the
      // user.
      //
      shared_ptr<repository> r (db.find<repository> (l.canonical_name ()));

      if (r == nullptr)
      {
        r = make_shared<repository> (l);
        db.persist (r); // Enter into session, important if recursive.
      }
      else if (r->location.url () != l.url ())
      {
        shared_ptr<repository_fragment> root (
          db.load<repository_fragment> (""));

        repository_fragment::dependencies& ua (root->complements);

        if (ua.find (r) == ua.end ())
        {
          r->location = l;
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

    // Note that it relies on the prerequisite and complement repositories be
    // already persisted.
    //
    add_trust ();

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
    // packages it contains. Note that when we fetch all the repositories the
    // available packages are cleaned up in advance (see rep_fetch() for
    // details).
    //
    if (exists && !full_fetch)
      rep_remove_package_locations (db, t, rf->name);

    vector<package_manifest>&   pms (fr.packages);
    const vector<package_info>& pis (fr.package_infos);

    for (size_t i (0); i != pms.size (); ++i)
    {
      package_manifest& pm (pms[i]);

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
            db,
            t,
            path_cast<dir_path> (rl.path () / *pm.location),
            pm.name,
            pm.version,
            !pis.empty () ? &pis[i] : nullptr,
            false   /* check_external */));

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

            diag_record dr (fail);

            dr << "checksum mismatch for " << pm.name << " " << pm.version <<
              info << r1 << " has " << *pm.sha256sum <<
              info << r2 << " has " << *p->sha256sum;

            // If we fetch all the repositories then the mismatch is
            // definitely caused by the broken repository. Otherwise, it may
            // also happen due to the old available package that is not wiped
            // out yet.  Thus, we advice the user to perform the full fetch,
            // unless the filesystem state is already changed and so this
            // advice will be given anyway (see rep_fetch() for details).
            //
            if (full_fetch)
              dr << info << "consider reporting this to the repository "
                         << "maintainers";
            else if (!filesystem_state_changed)
              dr << info << "run 'bpkg rep-fetch' to update";
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

  using repositories = set<shared_ptr<repository>>;

  // If reason is absent, then don't print the "fetching ..." progress line.
  //
  static void
  rep_fetch (const common_options& co,
             database& db,
             transaction& t,
             const shared_ptr<repository>& r,
             const optional<string>& dependent_trust,
             repositories& fetched_repositories,
             repositories& removed_repositories,
             repository_fragments& parsed_fragments,
             repository_fragments& removed_fragments,
             bool shallow,
             bool full_fetch,
             const optional<string>& reason)
  {
    tracer trace ("rep_fetch(rep)");

    tracer_guard tg (db, trace);

    // Check that the repository is not fetched yet and register it as fetched
    // otherwise.
    //
    // Note that we can end up with a repository dependency cycle via
    // prerequisites. Thus we register the repository before recursing into
    // its dependencies.
    //
    if (!fetched_repositories.insert (r).second) // Is already fetched.
    {
      // Authenticate the repository use by the dependent, if required.
      //
      // Note that we only need to authenticate the certificate but not the
      // repository that was already fetched (and so is already
      // authenticated).
      //
      if (need_auth (co, r->location))
        authenticate_certificate (co,
                                  &db.config_orig,
                                  &db,
                                  r->certificate,
                                  r->location,
                                  dependent_trust);

      return;
    }

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
    repository_fragment::dependencies old_complements;
    repository_fragment::dependencies old_prerequisites;

    auto collect_deps = [] (const shared_ptr<repository_fragment>& rf,
                            repository_fragment::dependencies& cs,
                            repository_fragment::dependencies& ps)
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
    // Note that we do this in the forward compatible manner ignoring
    // unrecognized manifest values and unsatisfied build2 toolchain
    // constraints in the package manifests. This approach allows older
    // toolchains to work with newer repositories, successfully building the
    // toolchain-satisfied packages and only failing for unsatisfied ones.
    //
    rep_fetch_data rfd (
      rep_fetch (co,
                 &db.config_orig,
                 &db,
                 rl,
                 dependent_trust,
                 true /* ignore_unknow */,
                 true /* ignore_toolchain */,
                 false /* expand_values */,
                 true /* load_buildfiles */));

    // Save for subsequent certificate authentication for repository use by
    // its dependents.
    //
    r->certificate = move (rfd.certificate_pem);

    repository_fragment::dependencies new_complements;
    repository_fragment::dependencies new_prerequisites;
    repository_trust repo_trust;

    for (rep_fetch_data::fragment& fr: rfd.fragments)
    {
      string nm (fr.friendly_name); // Don't move, still may be used.

      shared_ptr<repository_fragment> rf (rep_fragment (co,
                                                        db,
                                                        t,
                                                        rl,
                                                        move (fr),
                                                        parsed_fragments,
                                                        full_fetch,
                                                        repo_trust));

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
    {
      auto eq = [] (const lazy_weak_ptr<repository>& x,
                    const lazy_weak_ptr<repository>& y)
      {
        return x.object_id () == y.object_id ();
      };

      shallow = equal (new_complements.begin (), new_complements.end (),
                       old_complements.begin (), old_complements.end (),
                       eq) &&
                equal (new_prerequisites.begin (), new_prerequisites.end (),
                       old_prerequisites.begin (), old_prerequisites.end (),
                       eq);
    }

    // Fetch prerequisites and complements, unless this is a shallow fetch.
    //
    if (!shallow)
    {
      // Register old complements and prerequisites for potential removal
      // unless they are fetched.
      //
      auto rm = [&fetched_repositories, &removed_repositories] (
        const lazy_weak_ptr<repository>& rp)
      {
        shared_ptr<repository> r (rp.load ());
        if (fetched_repositories.find (r) == fetched_repositories.end ())
          removed_repositories.insert (move (r));
      };

      for (const lazy_weak_ptr<repository>& cr: old_complements)
      {
        // Remove the complement unless it is the root repository (see
        // rep_fetch() for details).
        //
        if (cr.object_id () != "")
          rm (cr);
      }

      for (const lazy_weak_ptr<repository>& pr: old_prerequisites)
        rm (pr);

      auto fetch = [&co,
                    &db,
                    &t,
                    &fetched_repositories,
                    &removed_repositories,
                    &parsed_fragments,
                    &removed_fragments,
                    full_fetch,
                    &rl,
                    &repo_trust]
        (const shared_ptr<repository>& r, const char* what)
      {
        auto i (repo_trust.find (r));
        assert (i != repo_trust.end ());

        rep_fetch (co,
                   db,
                   t,
                   r,
                   i->second,
                   fetched_repositories,
                   removed_repositories,
                   parsed_fragments,
                   removed_fragments,
                   false /* shallow */,
                   full_fetch,
                   what + rl.canonical_name ());
      };

      // Fetch complements and prerequisites.
      //
      for (const auto& cr: new_complements)
      {
        if (cr.object_id () != "")
        {
          fetch (cr.load (), "complements ");

          // Remove the repository from the prerequisites, if present, to avoid
          // the use re-authentication.
          //
          new_prerequisites.erase (cr);
        }
      }

      for (const auto& pr: new_prerequisites)
        fetch (pr.load (), "prerequisite of ");
    }
  }

  static void
  rep_fetch (const common_options& o,
             database& db,
             transaction& t,
             const vector<lazy_shared_ptr<repository>>& repos,
             bool shallow,
             bool full_fetch,
             const optional<string>& reason)
  {
    tracer trace ("rep_fetch(repos)");

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
      repository_fragments parsed_fragments;
      repository_fragments removed_fragments;

      // Fetch the requested repositories, recursively.
      //
      for (const lazy_shared_ptr<repository>& r: repos)
        rep_fetch (o,
                   db,
                   t,
                   r.load (),
                   nullopt /* dependent_trust */,
                   fetched_repositories,
                   removed_repositories,
                   parsed_fragments,
                   removed_fragments,
                   shallow,
                   full_fetch,
                   reason);

      // Remove dangling repositories.
      //
      for (const shared_ptr<repository>& r: removed_repositories)
      {
        // Prior to removing the repository we need to make sure it still
        // exists, which may not be the case due to earlier removal of the
        // dependent dangling repository.
        //
        if (db.find<repository> (r->name) != nullptr)
          rep_remove (db, t, r);
      }

      // Remove dangling repository fragments.
      //
      // Prior to removing a fragments we need to make sure it still exists,
      // which may not be the case due to the containing dangling repository
      // removal (see above).
      //
      for (const shared_ptr<repository_fragment>& rf: removed_fragments)
      {
        shared_ptr<repository_fragment> f (
          db.find<repository_fragment> (rf->name));

        if (f != nullptr)
        {
          // The persisted object must be the same as the one being removed.
          //
          assert (f == rf);

          rep_remove_fragment (db, t, rf);
        }
      }

#ifndef NDEBUG
      rep_remove_verify (db, t);
#endif

      // Make sure that the external packages are available from a single
      // directory-based repository.
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
            compare_version_eq (id.version,
                                ap.version,
                                true /* revision */,
                                false /* iteration */))
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

      // Finally, invert the main packages external test dependencies into the
      // the test packages special test dependencies.
      //
      // But first, remove the existing (and possibly outdated) special test
      // dependencies from the test packages, unless all the available
      // packages are (re)created from scratch.
      //
      if (!full_fetch)
      {
        for (const auto& at: db.query<available_test> ())
        {
          dependencies& ds (at.package->dependencies);

          // Note that there is only one special test dependencies entry in
          // the test package.
          //
          for (auto i (ds.begin ()), e (ds.end ()); i != e; ++i)
          {
            if (i->type)
            {
              ds.erase (i);
              break;
            }
          }

          db.update (at.package);
        }
      }

      // Go through the available packages that have external tests and add
      // them as the special test dependencies to these test packages.
      //
      // Note that not being able to resolve the test package for a main
      // package is not an error, since the test package absence doesn't
      // affect the main package building and internal testing. Dropping of an
      // external test package from a repository may, however, be intentional.
      // Think of a private repository crafted as a subset of some public
      // repository with the external examples packages omitted.
      //
      for (const auto& am: db.query<available_main> ())
      {
        const shared_ptr<available_package>& p (am.package);
        const package_name& n (p->id.name);
        const version& v (p->version);

        vector<shared_ptr<repository_fragment>> rfs;

        for (const package_location& pl: p->locations)
          rfs.push_back (pl.repository_fragment.load ());

        bool module (build2_module (n));

        for (const test_dependency& td: p->tests)
        {
          // Verify that the package has no runtime tests if it is a build
          // system module.
          //
          if (module && !td.buildtime)
            fail << "run-time " << td.type << ' ' << td.name << " for build "
                 << "system module "
                 << package_string (n, v) <<
              info << "build system modules cannot have run-time " << td.type;

          vector<pair<shared_ptr<available_package>,
                      shared_ptr<repository_fragment>>> tps (
                        filter (rfs,
                                query_available (db,
                                                 td.name,
                                                 td.constraint,
                                                 false /* order */),
                                false /* prereq */));

          for (const auto& t: tps)
          {
            const shared_ptr<available_package>& tp (t.first);

            dependencies& ds (tp->dependencies);

            // Find the special test dependencies entry, if already present.
            //
            auto b  (ds.begin ());
            auto e  (ds.end ());
            auto oi (b);           // Old entry location.
            for (; oi != e && !oi->type; ++oi) ;

            // Note that since we store all the primary packages as
            // alternative dependencies (which must be all of the same
            // dependency type) for the test package, it must either be a
            // runtime or build-time dependency for all of them.
            //
            // Note that the test package alternative dependencies contain the
            // `== <version>` constraints (see below), so we can use min
            // version of such a constraint as the primary package version.
            //
            if (oi != e && oi->buildtime != td.buildtime)
            {
              dependency_alternatives_ex& das (*oi);
              assert (!das.empty ()); // Cannot be empty if present.

              const dependency_alternative& da (das[0]);

              // We always add the primary package to the test package as a
              // single-dependency alternative (see below).
              //
              assert (da.size () == 1);

              fail << to_string (td.type) << " package " << td.name << " is a "
                   << "build-time dependency for one primary package and a "
                   << "run-time for another" <<
                info << (das.buildtime ? "build-time for " : "run-time for ")
                     << package_string (da[0].name,
                                        *da[0].constraint->min_version) <<
                info << (td.buildtime ? "build-time for " : "run-time for ")
                     << package_string (n, v);
            }

            // Find the (new) location for the special test dependencies entry.
            //
            // Note that if the entry is already present, it can only be moved
            // towards the end of the list.
            //
            auto ni (e);

            // First, find the last depends clause that explicitly specifies
            // this main package but goes after the special entry current
            // location, if present. Note that we only consider clauses with
            // the matching buildtime flag.
            //
            for (auto i (oi != e ? oi + 1 : b); i != e; ++i)
            {
              const dependency_alternatives_ex& das (*i);
              if (das.buildtime == td.buildtime)
              {
                bool specifies (false);

                for (const dependency_alternative& da: das)
                {
                  for (const dependency& d: da)
                  {
                    if (d.name == n)
                    {
                      specifies = true;
                      break;
                    }
                  }

                  if (specifies)
                    break;
                }

                if (specifies)
                  ni = i;
              }
            }

            // Now, set ni to refer to the special test dependencies entry,
            // moving or creating one, if required.
            //
            if (oi != e)   // The entry already exists?
            {
              if (ni != e) // Move the entry to the new location?
              {
                // Move the [oi + 1, ni] range 1 position to the left and
                // move the *oi element to the now vacant ni slot.
                //
                rotate (oi, oi + 1, ni + 1);
              }
              else
                ni = oi;   // Leave the entry at the old location.
            }
            else           // The entry doesn't exist.
            {
              if (ni != e) // Create the entry right after ni?
                ++ni;
              else
                ni = b;    // Create the entry at the beginning of the list.

              ni = ds.emplace (ni, td.type, td.buildtime); // Create the entry.
            }

            // Finally, add the new dependency alternative to the special
            // entry.
            //
            dependency_alternative da (td.enable,
                                       td.reflect,
                                       nullopt /* prefer */,
                                       nullopt /* accept */,
                                       nullopt /* require */);

            da.push_back (dependency {n, version_constraint (v)});

            assert (ni != ds.end ()); // Must be deduced by now.

            ni->push_back (move (da));

            db.update (tp);
          }
        }
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

        rep_remove_clean (o, db);
      }

      throw;
    }
  }

  void
  rep_fetch (const common_options& o,
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
    repository_fragment::dependencies& ua (root->complements);

    for (const repository_location& rl: rls)
    {
      lazy_shared_ptr<repository> r (db, rl.canonical_name ());

      // Add the repository, unless it is already a top-level one and has the
      // same location.
      //
      // Note that on Windows we can overwrite the local repository location
      // with the same location but some characters specified in a different
      // case, which is ok.
      //
      if (ua.find (r) == ua.end () || r.load ()->location.url () != rl.url ())
        rep_add (o, db, t, rl);

      repos.emplace_back (r);
    }

    rep_fetch (o, db, t, repos, shallow, false /* full_fetch */, reason);

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

    // Pre-attach the explicitly linked databases since we call
    // package_iteration().
    //
    database db (c, trace, true /* pre_attach */);

    transaction t (db);
    session s; // Repository dependencies can have cycles.

    shared_ptr<repository_fragment> root (db.load<repository_fragment> (""));

    // User-added repos.
    //
    repository_fragment::dependencies& ua (root->complements);

    optional<string> reason;
    bool full_fetch (!args.more ());

    if (full_fetch)
    {
      if (ua.empty ())
        fail << "configuration " << c << " has no repositories" <<
          info << "use 'bpkg rep-add' to add a repository";

      for (const lazy_weak_ptr<repository>& r: ua)
        repos.push_back (lazy_shared_ptr<repository> (r));

      // Always print "fetching ..." for complements of the root, even if
      // there is only one.
      //
      reason = "";

      // Cleanup the available packages in advance to avoid sha256sum mismatch
      // for packages being fetched and the old available packages, that are
      // not wiped out yet (see rep_fragment() for details).
      //
      db.erase_query<available_package> ();
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
            r = lazy_shared_ptr<repository> (db, rep_add (o, db, t, rl));
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

    rep_fetch (o, db, t, repos, o.shallow (), full_fetch, reason);

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
