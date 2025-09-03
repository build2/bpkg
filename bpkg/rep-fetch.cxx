// file      : bpkg/rep-fetch.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-fetch.hxx>

#include <map>
#include <set>

#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <bpkg/auth.hxx>
#include <bpkg/fetch.hxx>
#include <bpkg/rep-add.hxx>
#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/rep-remove.hxx>
#include <bpkg/pkg-verify.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/fetch-cache.hxx>
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
    tracer trace ("rep_fetch_pkg");

    // For the specified repository try to retrieve the repositories and
    // packages metadata from the fetch cache, if enabled. In the offline mode
    // fail if unable to do so (cache is disabled or there is no cached entry
    // for the repository).
    //
    // Note that it's tempting to fetch the signature manifest before opening
    // (locking) the cache if not in the offline mode, to keep it unlocked for
    // the duration of the potential download. However, we need to query the
    // cache entry first, since this fetch may not be necessary due to the
    // session. Also, closing the cache for the time we download the manifest
    // files feels not easy to implement due to the session. Thus, let's keep
    // it simple for now by opening the cache before the first manifest fetch
    // and keeping it open until the metadata is potentially updated. Plus,
    // we do cache garbage collection while downloading.
    //

    // If we fetch in the configuration but the database is not open yet
    // (rep-info case), then open it to get the fetch cache mode and stash it
    // to potentially reuse for authenticating the certificate.
    //
    unique_ptr<database> pdb;
    if (conf != nullptr && db == nullptr)
    {
      pdb.reset (new database (*conf,
                               trace,
                               false /* pre_attach */,
                               false /* sys_rep */));

      db = pdb.get ();
    }

    fetch_cache cache (co, db);
    optional<fetch_cache::loaded_pkg_repository_metadata> crm;

    if (cache.enabled ())
    {
      cache.open (trace);

      // Note that the fragment for pkg repository URLs is always nullopt, so
      // can use the repository URL as is.
      //
      // Note also that we cache both local and remote URLs since a local URL
      // could be on a network filesystem or some such.
      //
      crm = cache.load_pkg_repository_metadata (rl.url ());

      if (cache.offline () && !crm)
        fail << "no metadata in fetch cache for repository " << rl.url ()
             << " in offline mode" <<
          info << "consider turning offline mode off";
    }
    else if (cache.offline ())
      fail << "no way to obtain metadata for repository " << rl.url ()
           << " in offline mode with fetch cache disabled" <<
        info << "consider enabling fetch cache or turning offline mode off";

    // If the cached metadata is retrieved, determine which of the cached
    // metadata files we can use. Specifically:
    //
    // - If no checksums are specified, then we use both the cached
    //   repositories and packages manifest files without any up-to-dateness
    //   checks.
    //
    // - Otherwise (both checksums are specified), we fetch the signature
    //   manifest and check if the packages manifest checksum still matches
    //   the cached checksum, and use the cached manifest files if it does.
    //
    // - Otherwise (the packages manifest checksum doesn't match), we fetch
    //   the packages manifest and check if the repositories manifest checksum
    //   still matches the cached checksum, and use the cached repositories
    //   manifest file if it does.
    //
    // - Otherwise (none of the checksums match), we don't use the cached
    //   metadata files.
    //
    // Note that we either use both cached manifest files, or none of them, or
    // only the repositories manifest (and never the packages manifest alone).
    //
    path cached_repositories_path;
    path cached_packages_path;

    // While at it, stash all the fetched manifests for potential reuse.
    //
    optional<signature_manifest> sm;
    optional<pair<pkg_package_manifests, string /* checksum */>> pmc;

    if (crm)
    {
      if (crm->repositories_checksum.empty ())
      {
        cached_repositories_path = move (crm->repositories_path);
        cached_packages_path = move (crm->packages_path);

        // Valid cache.
        //
        if ((verb && !co.no_progress ()) || co.progress ())
        {
          text << "skipped validating " << rl.url () << " (cache, "
               << (cache.offline () ? "offline)" : "session)");
        }
      }
      else
      {
        // Cache to be validated.
        //
        if ((verb && !co.no_progress ()) || co.progress ())
          text << "validating " << rl.url () << " (cache)";

        // Otherwise, load_pkg_repository_metadata() would return the empty
        // manifest checksums and we wouldn't be here.
        //
        assert (!cache.offline ());

        cache.start_gc ();
        sm = pkg_fetch_signature (co, rl, true /* ignore_unknown */);
        cache.stop_gc ();

        if (sm->sha256sum == crm->packages_checksum)
        {
          cached_repositories_path = move (crm->repositories_path);
          cached_packages_path = move (crm->packages_path);
        }
        else
        {
          cache.start_gc ();
          pmc = pkg_fetch_packages (co, conf, rl, ignore_unknown);
          cache.stop_gc ();

          if (sm->sha256sum != pmc->second)
          {
            error << "packages manifest file checksum mismatch for "
                  << rl.canonical_name () <<
              info << "consider retrying this operation if this is a "
                   << "transient error" <<
              info << "consider reporting this to repository maintainers "
                   << "if this is a persistent error";

            throw recoverable ();
          }

          pkg_package_manifests& pms (pmc->first);

          if (pms.sha256sum == crm->repositories_checksum)
            cached_repositories_path = move (crm->repositories_path);
        }
      }
    }
    else
    {
      // Nothing in the cache, full fetch.
      //
      if ((verb && !co.no_progress ()) || co.progress ())
        text << "querying " << rl.url ();
    }

    rep_fetch_data::fragment fr;

    // Parse the repositories manifest file, by either using its cached
    // version or fetching it from the repository.
    //
    optional<pair<pkg_repository_manifests, string /* checksum */>> rmc;

    if (cached_repositories_path.empty ())
    {
      // Otherwise, we would fail earlier, if the cache is disabled or there
      // is no entry, or load_pkg_repository_metadata() would return the empty
      // manifest checksums, cached_repositories_path wouldn't be empty, and
      // so we wouldn't be here.
      //
      assert (!cache.offline ());

      if (cache.enabled ()) cache.start_gc ();
      rmc = pkg_fetch_repositories (co, rl, ignore_unknown);
      if (cache.enabled ()) cache.stop_gc ();

      fr.repositories = move (rmc->first);
    }
    else
    {
      fr.repositories = pkg_fetch_repositories (cached_repositories_path,
                                                ignore_unknown);
    }

    // Authenticate the repository certificate or, if unsigned, make sure the
    // repository is trusted.
    //
    // Note that if we use the cached packages manifest file, then we don't
    // authenticate the repository (nor certificate). Otherwise, if we use the
    // cached repositories manifest file, then we authenticate the repository
    // but not the certificate (since it is already cached as part of the
    // repositories manifest file). But we still need to verify the
    // certificate (validity period and such) in the latter case.
    //
    bool a (cached_packages_path.empty () && need_auth (co, rl));

    shared_ptr<const certificate> cert;
    optional<string> cert_pem (
      find_base_repository (fr.repositories).certificate);

    if (a)
    {
      if (cached_repositories_path.empty ())
      {
        cert = authenticate_certificate (
          co, db, &cache, cert_pem, rl, dependent_trust);
      }
      else
      {
        cert = cert_pem
          ? parse_certificate (co, *cert_pem, rl)
          : dummy_certificate (co, rl);

        verify_certificate (*cert, rl);
      }

      a = !cert->dummy ();
    }

    // Parse the packages manifest file, by either using its cached version or
    // fetching it from the repository.
    //
    if (cached_packages_path.empty ())
    {
      // Note that if we use the cached repositories manifest (rmc is absent),
      // then here we don't check its checksum against the one recorded in the
      // packages manifest. That have actually been already done when we
      // decided to use the cached repositories manifest. This also means that
      // the packages manifest have been already fetched (pmc is present).
      //
      assert (rmc || pmc);

      // If the packages manifest is not fetched yet, then fetch it and
      // verify that it matches the repositories manifest.
      //
      if (!pmc)
      {
        // Otherwise, we would fail earlier, if the cache is disabled or there
        // is no entry, or load_pkg_repository_metadata() would return the
        // empty manifest checksums, cached_packages_path wouldn't be empty,
        // and so we wouldn't be here.
        //
        assert (!cache.offline ());

        if (cache.enabled ()) cache.start_gc ();
        pmc = pkg_fetch_packages (co, conf, rl, ignore_unknown);
        if (cache.enabled ()) cache.stop_gc ();

        if (rmc->second != pmc->first.sha256sum)
        {
          error << "repositories manifest file checksum mismatch for "
                << rl.canonical_name () <<
            info << "consider retrying this operation if this is a "
                 << "transient error" <<
            info << "consider reporting this to repository maintainers "
                 << "if this is a persistent error";

          throw recoverable ();
        }
      }

      fr.packages = move (pmc->first);
    }
    else
    {
      fr.packages = pkg_fetch_packages (cached_packages_path, ignore_unknown);
    }

    // Authenticate the repository.
    //
    if (a)
    {
      if (!sm)
      {
        // Otherwise, we would fail earlier, if the cache is disabled or there
        // is no entry, or load_pkg_repository_metadata() would return the
        // empty manifest checksums, cached_packages_path wouldn't be empty,
        // the repository authentication wouldn't be necessary, and so we
        // wouldn't be here.
        //
        assert (!cache.offline ());

        if (cache.enabled ()) cache.start_gc ();
        sm = pkg_fetch_signature (co, rl, true /* ignore_unknown */);
        if (cache.enabled ()) cache.stop_gc ();
      }

      assert (pmc); // Wouldn't be here otherwise.

      if (sm->sha256sum != pmc->second)
      {
        error << "packages manifest file checksum mismatch for "
              << rl.canonical_name () <<
          info << "consider retrying this operation if this is a "
               << "transient error" <<
          info << "consider reporting this to repository maintainers "
               << "if this is a persistent error";

        throw recoverable ();
      }

      if (!sm->signature)
        fail << "no signature specified in signature manifest for signed "
             << rl.canonical_name () <<
          info << "consider reporting this to repository maintainers";

      assert (cert != nullptr); // Wouldn't be here otherwise.

      authenticate_repository (co, conf, cert_pem, *cert, *sm, rl);
    }

    // If the fetch cache is enabled, then save the fetched repositories and
    // packages manifests, if any, into the cache and close (release) the
    // cache.
    //
    if (cache.enabled ())
    {
      // We either use a cached manifest file or we fetch it.
      //
      assert (cached_repositories_path.empty () == rmc.has_value () &&
              cached_packages_path.empty ()     == pmc.has_value ());

      if (pmc)
      {
        fetch_cache::saved_pkg_repository_metadata srm (
          cache.save_pkg_repository_metadata (
            rl.url (),
            rmc ? move (rmc->second) : string (),
            move (pmc->second)));

        // Note: use the "write to temporary and atomically move into place"
        // technique.

        // repositories.manifest
        //
        if (!srm.repositories_path.empty ())
        {
          auto_rmfile arm (srm.repositories_path + ".tmp");
          const path& p (arm.path);

          try
          {
            // Let's set the binary mode not to litter the manifest file
            // with the carriage return characters on Windows.
            //
            ofdstream ofs (p, fdopen_mode::binary);
            manifest_serializer s (ofs, p.string ());

            assert (rmc); // Wouldn't be here otherwise.

            pkg_repository_manifests& rms (rmc->first);

            // Temporarily restore the manifests list in the fetched list
            // manifest.
            //
            static_cast<vector<repository_manifest>&> (rms) =
              move (fr.repositories);

            rms.serialize (s);
            fr.repositories = move (rms);

            ofs.close ();
          }
          catch (const io_error& e)
          {
            fail << "unable to write to " << p << ": " << e;
          }

          mv (p, srm.repositories_path);
          arm.cancel ();
        }

        // packages.manifest
        //
        {
          auto_rmfile arm (srm.packages_path + ".tmp");
          const path& p (arm.path);

          try
          {
            ofdstream ofs (p, fdopen_mode::binary);
            manifest_serializer s (ofs, p.string ());

            pkg_package_manifests& pms (pmc->first);

            static_cast<vector<package_manifest>&> (pms) = move (fr.packages);

            pms.serialize (s);
            fr.packages = move (pms);

            ofs.close ();
          }
          catch (const io_error& e)
          {
            fail << "unable to write to " << p << ": " << e;
          }

          mv (p, srm.packages_path);
          arm.cancel ();
        }
      }

      cache.close ();
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

  // Return the fetched repository data together with the total number of the
  // packages in the fetched repository. Return nullopt, if the underlying
  // git_fetch() call returned nullopt.
  //
  static optional<pair<rep_fetch_data, size_t>>
  rep_fetch_git (const common_options& co,
                 fetch_cache& cache,
                 const repository_location& rl,
                 const dir_path& rd,
                 bool init,
                 const path& ls_remote,
                 bool iu,
                 bool it,
                 bool ev,
                 bool lb)
  {
    // Initialize a new repository in the specified directory.
    //
    if (init)
      git_init (co, rl, rd);

    // Fetch the repository in the specified directory.
    //
    optional<vector<git_fragment>> frags (
      git_fetch (co, cache, rl, rd, ls_remote));

    if (!frags)
      return nullopt;

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

    for (git_fragment& gf: *frags)
    {
      git_checkout (co, rd, gf.commit);

      rep_fetch_data::fragment fr;
      fr.id            = move (gf.commit);
      fr.friendly_name = move (gf.friendly_name);

      // Parse repository manifests.
      //
      fr.repositories = parse_repository_manifests<git_repository_manifests> (
          rd / repositories_file,
          iu,
          rl,
          fr.friendly_name);

      // Parse package skeleton manifests.
      //
      git_package_manifests pms (
        parse_directory_manifests<git_package_manifests> (
          rd / packages_file,
          iu,
          rl,
          fr.friendly_name));

      // Checkout submodules on the first call.
      //
      bool cs (true);
      auto checkout_submodules = [&co, &cache, &rl, &rd, &cs] ()
      {
        if (cs)
        {
          cs = false;
          return git_checkout_submodules (co, cache, rl, rd);
        }

        return true;
      };

      // Checkout submodules to parse package manifests, if required.
      //
      for (const package_manifest& sm: pms)
      {
        dir_path d (rd / path_cast<dir_path> (*sm.location));

        if (!exists (d) || empty (d))
        {
          // To fully conform to the function description we should probably
          // throw failed here if git_checkout_submodules() returns false,
          // since the root repository has been fetched, its state has
          // changed, etc. However, let's return nullopt (as if the root
          // repository fetch has not started) not to, for example, remove the
          // cleanly fetched root repository state just because some of its
          // submodule repositories may not be available at the moment.
          //
          if (!checkout_submodules ())
            return nullopt;

          break;
        }
      }

      // Parse package manifests.
      //
      pair<vector<package_manifest>, vector<package_info>> pmi (
        parse_package_manifests (co,
                                 rd,
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
            bool bail (false);
            m.load_files (
              [ev, &rd, &rl, &pl, &fr, &checkout_submodules, &bail]
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
                  if (!exists (rd / pl / p))
                  {
                    if (!checkout_submodules ())
                    {
                      bail = true;
                      return nullopt;
                    }
                  }

                  return read_package_file (p,
                                            n,
                                            pl,
                                            rd,
                                            rl,
                                            fr.friendly_name);
                }
                else
                  return nullopt;
              },
              iu);

            if (bail)
              return nullopt;
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
            load_package_buildfiles (m, rd / pl, true /* err_path_relative */);
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

    return make_pair (move (r), np);
  }

  static rep_fetch_data
  rep_fetch_git (const common_options& co,
                 const dir_path* conf,
                 database* db,
                 const repository_location& rl,
                 bool iu,
                 bool it,
                 bool ev,
                 bool lb)
  {
    tracer trace ("rep_fetch_git");

    // If we fetch in the configuration but the database is not open yet
    // (rep-info case), then open it to get the fetch cache mode.
    //
    unique_ptr<database> pdb;
    if (conf != nullptr && db == nullptr)
    {
      pdb.reset (new database (*conf,
                               trace,
                               false /* pre_attach */,
                               false /* sys_rep */));

      db = pdb.get ();
    }

    dir_path sd (repository_state (rl));

    dir_path rsd;
    dir_path rd;

    if (conf != nullptr)
    {
      rsd = *conf / repos_dir;
      rd = rsd / sd;

      // Convert the 12 characters checksum abbreviation to 16 characters for
      // the repository directory names.
      //
      // @@ TMP Remove this some time after the toolchain 0.18.0 is released.
      //
      {
        const string& s (rd.string ());
        dir_path d (s, s.size () - 4);

        if (exists (d))
          mv (d, rd);
      }
    }

    bool config_repo_exists (!rd.empty () && exists (rd));

    optional<pair<rep_fetch_data, size_t>> r;

    // NOTE: keep the subsequent fetch logic of using fetch cache and
    //       configuration-specific repository cache parallel.

    fetch_cache cache (co, db);

    if (cache.enabled ())
    {
      cache.open (trace);

      // Remove the configuration-specific repository directory, if exists.
      // Essentially we are switching to the global cached one.
      //
      if (config_repo_exists)
      {
        filesystem_state_changed = true;

        rm_r (rd);
      }

      // Note that we cache both local and remote URLs since a local URL could
      // be on a network filesystem or some such.
      //
      repository_url url (rl.url ());
      url.fragment = nullopt;

      fetch_cache::loaded_git_repository_state crs (
        cache.load_git_repository_state (url));

      const dir_path& td (crs.repository);

      // If the repository is already cached, then we are fetching an already
      // existing repository, moved to the temporary directory first.
      // Otherwise, we initialize the repository in the temporary directory,
      // unless we are in the offline mode in which case we fail.
      //
      // In the former case, unless offline, also set the
      // filesystem_state_changed flag since we are modifying the repository
      // filesystem state.
      //
      bool repo_cached (
        crs.state != fetch_cache::loaded_git_repository_state::absent);

      bool fsc (filesystem_state_changed);

      if (repo_cached)
      {
        if (!cache.offline ())
          filesystem_state_changed = true;
      }
      else if (cache.offline ())
        fail << "no state in fetch cache for repository " << rl.url ()
             << " in offline mode" <<
          info << "consider turning offline mode off";

      // If this call throws failed, then the cache entry is not saved.
      // Otherwise, save the entry if the fetch succeeded or didn't start (no
      // connectivity, etc) for an already cached repository. Otherwise
      // (fetching of new repository didn't start), don't save the cache
      // entry.
      //
      r = rep_fetch_git (co,
                         cache,
                         rl,
                         td,
                         !repo_cached /* initialize */,
                         crs.ls_remote,
                         iu,
                         it,
                         ev,
                         lb);

      // Remove the working tree from the state directory and save it to the
      // cache.
      //
      if (r || repo_cached)
      {
        git_remove_worktree (co, td);

        cache.save_git_repository_state (move (url));
      }

      cache.close ();

      // If the cached repository is saved without being fetched, then revert
      // the filesystem_state_changed flag.
      //
      if (repo_cached && !r)
        filesystem_state_changed = fsc;

      // Fail for incomplete fetch.
      //
      if (!r)
        throw failed (); // Note that the diagnostics has already been issued.
    }
    else
    {
      if (cache.offline ())
        fail << "no way to obtain state for repository " << rl.url ()
             << " in offline mode with fetch cache disabled" <<
          info << "consider enabling fetch cache or turning offline mode off";

      auto i (tmp_dirs.find (conf != nullptr ? *conf : empty_dir_path));
      assert (i != tmp_dirs.end ());

      auto_rmdir rm (i->second / sd, !keep_tmp);
      const dir_path& td (rm.path);

      if (exists (td))
        rm_r (td);

      // If the git repository directory already exists, then we are fetching
      // an already existing repository, moved to the temporary directory
      // first. Otherwise, we initialize the repository in the temporary
      // directory.
      //
      // In the former case also set the filesystem_state_changed flag since
      // we are modifying the repository filesystem state.
      //
      bool fsc (filesystem_state_changed);
      if (config_repo_exists)
      {
        mv (rd, td);
        filesystem_state_changed = true;
      }

      // If this call throws failed, then the temporary state directory is
      // removed. Otherwise, return it to its permanent location if the fetch
      // succeeded or didn't start (no connectivity, etc) for an existing
      // repository. Otherwise (fetching of new repository didn't start),
      // remove the temporary state.
      //
      r = rep_fetch_git (co,
                         cache,
                         rl,
                         td,
                         !config_repo_exists /* initialize */,
                         path () /* ls_remote */,
                         iu,
                         it,
                         ev,
                         lb);

      // Remove the working tree from the state directory and return it to its
      // permanent location.
      //
      // If there is no configuration directory, then we let auto_rmdir clean
      // it up from the temporary directory.
      //
      if (!rd.empty ())
      {
        if (r || config_repo_exists)
        {
          git_remove_worktree (co, td);

          // Make sure the repos/ directory exists (could potentially be
          // manually removed).
          //
          if (!exists (rsd))
            mk (rsd);

          mv (td, rd);
          rm.cancel ();
        }

        // If the existing repository is returned to its permanent location
        // without being fetched, then revert the filesystem_state_changed
        // flag.
        //
        if (config_repo_exists && !r)
          filesystem_state_changed = fsc;
      }

      // Fail for incomplete fetch.
      //
      if (!r)
        throw failed (); // Note that the diagnostics has already been issued.
    }

    if (r->second == 0 && !rl.url ().fragment)
      warn << "repository " << rl << " has no available packages" <<
        info << "consider specifying explicit URL fragment (for example, "
             << "#master)";

    return move (r->first);
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
             bool lb,
             const string& reason,
             bool no_dir_progress)
  {
    if (verb && (!no_dir_progress || !rl.directory_based ()))
    {
      diag_record dr (text);
      dr << "fetching " << rl.canonical_name ();

      if (!reason.empty ())
        dr << " (" << reason << ")";
    }

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
        return rep_fetch_git (co, conf, db, rl, iu, it, ev, lb);
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
                      lb,
                      "" /* reason */,
                      false /* no_dir_progress */);
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
        db.find<available_package> (package_id (pm.name, pm.version)));

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
            // out yet. Thus, we advice the user to perform the full fetch,
            // unless the filesystem state is already changed and so this
            // advice will be given anyway (see rep_fetch() for details).
            //
            if (full_fetch)
              dr << info << "consider reporting this to repository maintainers";
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
             const string& reason,
             bool no_dir_progress)
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
      {
        authenticate_certificate (co,
                                  &db,
                                  nullptr /* fetch_cache */,
                                  r->certificate,
                                  r->location,
                                  dependent_trust);
      }

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
                 true /* load_buildfiles */,
                 reason,
                 no_dir_progress));

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
                   what + rl.canonical_name (),
                   false /* no_dir_progress */);
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
             const string& reason,
             bool no_dir_progress)
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
                   reason,
                   no_dir_progress);

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

      package_id ap;
      shared_ptr<repository_fragment> rf;

      for (const auto& prf: db.query<package_repository_fragment> (q))
      {
        const shared_ptr<repository_fragment>& f (prf.repository_fragment);
        if (!f->location.directory_based ())
          continue;

        // Fail if the external package is of the same upstream version and
        // revision as the previous one.
        //
        const package_id& id (prf.package_id);

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
             bool shallow)
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

    rep_fetch (o,
               db,
               t,
               repos,
               shallow,
               false /* full_fetch */,
               "" /* reason */,
               false /* no_dir_progress */);

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
    database db (c, trace, true /* pre_attach */, false /* sys_rep */);

    transaction t (db);
    session s; // Repository dependencies can have cycles.

    shared_ptr<repository_fragment> root (db.load<repository_fragment> (""));

    // User-added repos.
    //
    repository_fragment::dependencies& ua (root->complements);

    bool full_fetch (!args.more ());

    if (full_fetch)
    {
      if (ua.empty ())
        fail << "configuration " << c << " has no repositories" <<
          info << "use 'bpkg rep-add' to add a repository";

      for (const lazy_weak_ptr<repository>& r: ua)
        repos.push_back (lazy_shared_ptr<repository> (r));

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
    }

    rep_fetch (o,
               db,
               t,
               repos,
               o.shallow (),
               full_fetch,
               "" /* reason */,
               o.no_dir_progress ());

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
