// file      : bpkg/pkg-fetch.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-fetch.hxx>

#include <libbutl/filesystem.hxx> // mkhardlink(), cpfile()

#include <libbpkg/manifest.hxx>

#include <bpkg/fetch.hxx>
#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/checksum.hxx>
#include <bpkg/database.hxx>
#include <bpkg/rep-mask.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/fetch-cache.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/pkg-purge.hxx>
#include <bpkg/pkg-verify.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  // Return the selected package object which may replace the existing one.
  //
  static shared_ptr<selected_package>
  pkg_fetch (database& db,
             transaction& t,
             package_name n,
             version v,
             path a,
             repository_location rl,
             string m,
             bool purge,
             bool simulate)
  {
    tracer trace ("pkg_fetch");

    tracer_guard tg (db, trace);

    // Make the archive path absolute and normalized. If the archive is
    // inside the configuration, use the relative path. This way we can move
    // the configuration around.
    //
    normalize (a, "archive");

    // Only purge the existing archive if its path differs from the new path.
    //
    shared_ptr<selected_package> p (db.find<selected_package> (n));

    bool purge_archive (p != nullptr &&
                        p->archive   &&
                        p->effective_archive (db.config) != a);

    if (a.sub (db.config))
      a = a.leaf (db.config);

    if (p != nullptr)
    {
      // Clean up the source directory and archive of the package we are
      // replacing. Once this is done, there is no going back. If things go
      // badly, we can't simply abort the transaction.
      //
      pkg_purge_fs (db, t, p, simulate, purge_archive);

      // Note that if the package name spelling changed then we need to update
      // it, to make sure that the subsequent commands don't fail and the
      // diagnostics is not confusing. However, we cannot update the object
      // id, so have to erase it and persist afterwards.
      //
      if (p->name.string () != n.string ())
      {
        db.erase (p);
        p = nullptr;
      }
    }

    if (p != nullptr)
    {
      p->version = move (v);
      p->state = package_state::fetched;
      p->repository_fragment = move (rl);
      p->archive = move (a);
      p->purge_archive = purge;
      p->manifest = move (m);

      // Mark the section as loaded, so the manifest is updated.
      //
      p->manifest_section.load ();

      db.update (p);
    }
    else
    {
      // Add the package to the configuration.
      //
      p.reset (new selected_package {
        move (n),
        move (v),
        package_state::fetched,
        package_substate::none,
        false,   // hold package
        false,   // hold version
        move (rl),
        move (a),
        purge,
        nullopt, // No source directory yet.
        false,
        nullopt, // No manifest checksum.
        nullopt, // No buildfiles checksum.
        nullopt, // No output directory yet.
        {},      // No prerequisites captured yet.
        move (m)});

      db.persist (p);
    }

    t.commit ();
    return p;
  }

  // Check if the package already exists in this configuration and
  // diagnose all the illegal cases. We want to do this as soon as
  // the package name is known which happens at different times
  // depending on whether we are dealing with an existing archive
  // or fetching one.
  //
  static void
  pkg_fetch_check (database& db,
                   transaction&,
                   const package_name& n,
                   bool replace)
  {
    tracer trace ("pkg_fetch_check");

    tracer_guard tg (db, trace);

    if (shared_ptr<selected_package> p = db.find<selected_package> (n))
    {
      bool s (p->state == package_state::fetched ||
              p->state == package_state::unpacked);

      if (!replace || !s)
      {
        diag_record dr (fail);
        const dir_path& c (db.config_orig);

        dr << "package " << n << " already exists in configuration " << c <<
          info << "version: " << p->version_string ()
           << ", state: " << p->state
           << ", substate: " << p->substate;

        if (s) // Suitable state for replace?
          dr << info << "use 'pkg-fetch --replace|-r' to replace";
      }
    }
  }

  shared_ptr<selected_package>
  pkg_fetch (const common_options& co,
             database& db,
             transaction& t,
             path a,
             bool replace,
             bool purge,
             bool simulate)
  {
    tracer trace ("pkg_fetch");

    if (!exists (a))
      fail << "archive file '" << a << "' does not exist";

    l4 ([&]{trace << "archive: " << a << ", purge: " << purge;});

    // Verify archive is a package and get its manifest.
    //
    package_manifest m (pkg_verify (co,
                                    a,
                                    true /* ignore_unknown */,
                                    false /* ignore_toolchain */,
                                    false /* expand_values */,
                                    true /* load_buildfiles */));

    l4 ([&]{trace << m.name << " " << m.version;});

    // Check/diagnose an already existing package.
    //
    pkg_fetch_check (db, t, m.name, replace);

    // Create the temporary available package object from the package manifest
    // to serialize it into the available package manifest string.
    //
    available_package ap (move (m));
    string s (ap.manifest ());

    // Use the special root repository fragment as the repository fragment of
    // this package.
    //
    return pkg_fetch (db,
                      t,
                      move (ap.id.name),
                      move (ap.version),
                      move (a),
                      repository_location (),
                      move (s),
                      purge,
                      simulate);
  }

  shared_ptr<selected_package>
  pkg_fetch (const common_options& co,
             database& pdb,
             database& rdb,
             transaction& t,
             package_name n,
             version v,
             bool replace,
             bool simulate)
  {
    assert (session::has_current ());

    tracer trace ("pkg_fetch");

    tracer_guard tg (pdb, trace); // NOTE: sets tracer for the whole cluster.

    // Check/diagnose an already existing package.
    //
    pkg_fetch_check (pdb, t, n, replace);

    check_any_available (rdb, t);

    // Note that here we compare including the revision (unlike, say in
    // pkg-status). Which means one cannot just specify 1.0.0 and get 1.0.0+1
    // -- they must spell it out explicitly. This is probably ok since this is
    // a low-level command where some extra precision doesn't hurt.
    //
    package_id pid (n, v);
    shared_ptr<available_package> ap (rdb.find<available_package> (pid));

    if (ap == nullptr)
      fail << "package " << n << " " << v << " is not available";

    // Pick an archive-based repository fragment. Preferring a local one over
    // the remotes seems like a sensible thing to do.
    //
    const package_location* pl (nullptr);

    for (const package_location& l: ap->locations)
    {
      if (!rep_masked_fragment (l.repository_fragment))
      {
        const repository_location& rl (l.repository_fragment.load ()->location);

        if (rl.archive_based () && (pl == nullptr || rl.local ()))
        {
          pl = &l;

          if (rl.local ())
            break;
        }
      }
    }

    if (pl == nullptr)
      fail << "package " << n << " " << v
           << " is not available from an archive-based repository";

    if (verb > 1 && !simulate)
      text << "fetching " << pl->location.leaf () << " "
           << "from " << pl->repository_fragment->name;
    else
      l4 ([&]{trace << pl->location.leaf () << " from "
                    << pl->repository_fragment->name << pdb;});

    auto_rmfile arm;
    path an (pl->location.leaf ());
    path a (pdb.config_orig / an);

    // Note that in the replace mode we first fetch the new package version
    // archive and then update the existing selected package object, dropping
    // the previous package version archive, if present. This way we, in
    // particular, keep the existing selected package/archive intact if the
    // fetch operation fails. However, this approach requires to handle
    // re-fetching (potentially from a different repository) of the same
    // package version specially.
    //
    // Specifically, if we need to overwrite the package archive file, then we
    // stash the existing archive in the temporary directory and remove it on
    // success. On failure, we try to move the stashed archive to the original
    // place. Failed that either, we mark the package as broken.
    //
    // (If you are wondering why don't we instead always fetch into a
    // temporary file, the answer is Windows, where moving a newly created
    // file may not succeed because it is being scanned by Windows Defender
    // or some such.)
    //
    auto_rmfile earm;
    shared_ptr<selected_package> sp;

    auto g (
      make_exception_guard (
        [&arm, &a, &earm, &sp, &pdb, &t] ()
        {
          // Restore stashed archive.
          //
          if (!earm.path.empty () && exists (earm.path))
          {
            if (mv (earm.path, a, false /* fail */))
            {
              earm.cancel ();
              arm.cancel ();  // Note: may not be armed yet, which is ok.
            }
            //
            // Note: can already be marked as broken by pkg_purge_fs().
            //
            else if (sp->state != package_state::broken)
            {
              sp->state = package_state::broken;
              pdb.update (sp);
              t.commit ();

              // Here we assume that mv() has already issued the diagnostics.
              //
              info << "package " << sp->name << pdb << " is now broken; "
                   << "use 'pkg-purge --force' to remove";
            }
          }
        }));

    const repository_location& rl (pl->repository_fragment->location);

    bool purge (true);

    if (!simulate)
    {
      // Stash the existing package archive if it needs to be overwritten (see
      // above for details).
      //
      // Note: compare the archive absolute paths.
      //
      if (replace                                          &&
          (sp = pdb.find<selected_package> (n)) != nullptr &&
          sp->archive                                      &&
          sp->effective_archive (pdb.config) == pdb.config / an)
      {
        earm = tmp_file (pdb.config_orig, n.string () + '-' + v.string ());
        mv (a, earm.path);
      }

      // Try to create a hard link to a file and, if that fails, copy this
      // file to the link location. In the latter case, use the "write to
      // temporary and atomically move into place" technique.
      //
      auto hardlink = [] (const path& target, const path& link)
      {
        // Note that this implementation is inspired by libbutl's mkanylink()
        // function.
        //
        try
        {
          mkhardlink (target, link);
        }
        catch (const system_error& e)
        {
          if (e.code ().category () == generic_category ())
          {
            int c (e.code ().value ());
            if (c == ENOSYS || // Not implemented.
                c == EPERM  || // Not supported by the filesystem(s).
                c == EXDEV)    // On different filesystems.
            {
              auto_rmfile arm (link + ".tmp");
              const path& p (arm.path);

              try
              {
                cpfile (target, p);
              }
              catch (const system_error& e)
              {
                fail << "unable to copy file " << target << " to " << p
                     << ": " << e;
              }

              mv (p, link);
              arm.cancel ();
              return;
            }
          }

          fail << "unable to create hard link for " << target << " at "
               << link << ": " << e;
        }
      };

      // For the specified package version try to retrieve the archive file
      // from the fetch cache, if enabled. In the offline mode fail if unable
      // to do so (cache is disabled or there is no cached entry for the
      // package version).
      //
      fetch_cache cache (co, &pdb);
      optional<fetch_cache::loaded_pkg_repository_package> crp;

      if (cache.enabled ())
      {
        cache.open (trace);

        crp = cache.load_pkg_repository_package (pid);

        if (cache.offline () && !crp)
          fail << "no archive in fetch cache for package " << n << ' ' << v
               << " in offline mode" <<
            info << "consider turning offline mode off";
      }
      else if (cache.offline ())
        fail << "no way to obtain package " << n << ' ' << v
             << " in offline mode with fetch cache disabled" <<
          info << "consider enabling fetch cache or turning offline mode off";

      // Add the package archive file to the configuration, by either using
      // its cached version in place or fetching it from the repository.
      //
      // Should we close (unlock) the cache for the time we download the
      // archive? Let's keep it locked not to download same archive multiple
      // times (note: the probability of that is higher the larger the archive
      // size). Plus, we do cache garbage collection while downloading.
      //
      string fcs; // Fetched archive checksum.

      // We can't be fetching an archive for a transient object.
      //
      assert (ap->sha256sum);

      if (!crp)
      {
        pkg_fetch_archive (co, rl, pl->location, a);

        arm = auto_rmfile (a);

        fcs = sha256sum (co, a);

        if (fcs != *ap->sha256sum)
        {
          fail << "checksum mismatch for " << n << " " << v <<
            info << pl->repository_fragment->name << " has " << *ap->sha256sum <<
            info << "fetched archive has " << fcs <<
            info << "consider re-fetching package list and trying again" <<
            info << "if problem persists, consider reporting this to "
                 << "the repository maintainer";
        }
      }
      else
      {
        path& ca (crp->archive);

        // Note that currently there is no scenario when the archive name, as
        // it comes from a repository, doesn't match the one from the cache.
        // Let's, however, verify that for good measure.
        //
        if (an != ca.leaf ())
        {
          fail << "cached archive name " << ca.leaf () << " doesn't match "
               << "fetched archive name " << an <<
            info << "fetched archive repository: " << rl.url () <<
            info << "cached archive repository: " << crp->repository;
        }

        // Issue a warning if the checksum of the cached archive differs from
        // that one of the archive in the repository.
        //
        if (crp->checksum != *ap->sha256sum)
        {
          fail << "cached archive checksum " << crp->checksum << " doesn't "
               << "match fetched archive checksum " << *ap->sha256sum <<
            info << "fetched archive repository: " << rl.url () <<
            info << "cached archive repository: " << crp->repository;
        }

        // If sharing of the cached source directories is enabled, then use
        // the package archive in place from the cache and don't remove it
        // when the package is purged. Otherwise, hardlink/copy the archive
        // from the cache into the configuration directory.
        //
        if (cache.cache_src ())
        {
          a = move (ca);
          purge = false;
        }
        else
        {
          hardlink (ca, a);

          arm = auto_rmfile (a);
        }
      }

      // If the fetch cache is enabled, then save the package archive, if we
      // fetched it, into the cache and close (release) the cache.
      //
      if (cache.enabled ())
      {
        if (!crp)
        {
          // Note that the fragment for pkg repository URLs is always nullopt,
          // so can use the repository URL as is.
          //
          // Note also that we cache both local and remote URLs since a local
          // URL could be on a network filesystem or some such.
          //
          path ca (cache.save_pkg_repository_package (move (pid),
                                                      v,
                                                      move (an),
                                                      move (fcs),
                                                      rl.url ()));

          // If sharing of the cached source directories is enabled, then move
          // the package archive to the fetch cache, use it in place (from the
          // cache) in the configuration, and don't remove it when the package
          // is purged. Otherwise, hardlink/copy the archive from the
          // configuration directory into the cache.
          //
          if (cache.cache_src ())
          {
            // Note that the move operation can fallback to copy, if the
            // source and destination paths belong to different filesystems.
            // Thus, to implement the "write to temporary and atomically move
            // into place" technique, we move the archive in two steps: first,
            // to the destination filesystem under the temporary name and then
            // rename it to the final name.
            //
            auto_rmfile rm (ca + ".tmp");
            const path& p (rm.path);
            mv (a, p);
            mv (p, ca);
            rm.cancel ();

            a = move (ca);
            purge = false;
          }
          else
          {
            hardlink (a, ca);
          }
        }

        cache.close ();
      }
    }

    // Make sure all the available package sections, required for generating
    // the manifest, are loaded.
    //
    if (!ap->languages_section.loaded ())
      rdb.load (*ap, ap->languages_section);

    shared_ptr<selected_package> p (
      pkg_fetch (pdb,
                 t,
                 move (n),
                 move (v),
                 move (a),
                 rl,
                 ap->manifest (),
                 purge,
                 simulate));

    arm.cancel ();
    return p;
  }

  int
  pkg_fetch (const pkg_fetch_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_fetch");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    database db (c, trace, true /* pre_attach */, false /* sys_rep */);
    transaction t (db);
    session s;

    shared_ptr<selected_package> p;

    // pkg_fetch() in both cases commits the transaction.
    //
    if (o.existing ())
    {
      if (!args.more ())
        fail << "archive path argument expected" <<
          info << "run 'bpkg help pkg-fetch' for more information";

      p = pkg_fetch (o,
                     db,
                     t,
                     path (args.next ()),
                     o.replace (),
                     o.purge (),
                     false /* simulate */);
    }
    else
    {
      if (!args.more ())
        fail << "package name/version argument expected" <<
          info << "run 'bpkg help pkg-fetch' for more information";

      const char*  arg (args.next ());
      package_name n   (parse_package_name (arg));
      version      v   (parse_package_version (arg));

      if (v.empty ())
        fail << "package version expected" <<
          info << "run 'bpkg help pkg-fetch' for more information";

      p = pkg_fetch (o,
                     db /* pdb */,
                     db /* rdb */,
                     t,
                     move (n),
                     move (v),
                     o.replace (),
                     false /* simulate */);
    }

    if (verb && !o.no_result ())
    {
      if (!o.existing ())
        text << "fetched " << *p;
      else
        text << "using " << *p << " (external)";
    }

    return 0;
  }

  pkg_fetch_options
  merge_options (const default_options<pkg_fetch_options>& defs,
                 const pkg_fetch_options& cmd)
  {
    // NOTE: remember to update the documentation if changing anything here.

    return merge_default_options (
      defs,
      cmd,
      [] (const default_options_entry<pkg_fetch_options>& e,
          const pkg_fetch_options&)
      {
        const pkg_fetch_options& o (e.options);

        auto forbid = [&e] (const char* opt, bool specified)
        {
          if (specified)
            fail (e.file) << opt << " in default options file";
        };

        forbid ("--directory|-d", o.directory_specified ());
        forbid ("--purge|-p",     o.purge ()); // Dangerous.
      });
  }
}
