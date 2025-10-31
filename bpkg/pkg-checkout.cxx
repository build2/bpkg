// file      : bpkg/pkg-checkout.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-checkout.hxx>

#include <libbpkg/manifest.hxx>

#include <bpkg/fetch.hxx>            // git_checkout*()
#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/checksum.hxx>
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
  // pkg_checkout()
  //
  // Return false, if the underlying git_checkout_submodules() call returned
  // false.
  //
  static bool
  checkout (const common_options& o,
            fetch_cache& cache,
            const repository_location& rl,
            const dir_path& dir)
  {
    switch (rl.type ())
    {
    case repository_type::git:
      {
        assert (rl.fragment ());

        git_checkout (o, dir, *rl.fragment ());

        if (exists (dir / path (".gitmodules")))
        {
          if (!git_checkout_submodules (o, cache, rl, dir))
            return false;
        }

        break;
      }
    case repository_type::pkg:
    case repository_type::dir: assert (false); break;
    }

    return true;
  }

  // For some platforms/repository types the working tree needs to be
  // temporary "fixed up" for the build2 operations to work properly on it.
  //
  static optional<bool>
  fixup (const common_options& o,
         const repository_location& rl,
         const dir_path& dir,
         bool revert = false,
         bool fail = true)
  {
    optional<bool> r;

    switch (rl.type ())
    {
    case repository_type::git:
      {
        if (!revert && fail)
          git_verify_symlinks (o, dir);

        r = git_fixup_worktree (o, dir, revert, fail);
        break;
      }
    case repository_type::pkg:
    case repository_type::dir: assert (false); break;
    }

    return r;
  }

  // Return the selected package object which may replace the existing one.
  //
  static shared_ptr<selected_package>
  pkg_checkout (const common_options& o,
                bpkg::fetch_cache& fetch_cache,
                pkg_checkout_cache& checkout_cache,
                database& pdb,
                database& rdb,
                transaction& t,
                package_name n,
                version v,
                const optional<dir_path>& output_root,
                bool replace,
                bool purge,
                bool simulate)
  {
    tracer trace ("pkg_checkout");

    tracer_guard tg (pdb, trace); // NOTE: sets tracer for the whole cluster.

    const dir_path& c (pdb.config_orig);

    // See if this package already exists in this configuration.
    //
    shared_ptr<selected_package> p (pdb.find<selected_package> (n));

    if (p != nullptr)
    {
      bool s (p->state == package_state::fetched ||
              p->state == package_state::unpacked);

      if (!replace || !s)
      {
        diag_record dr (fail);

        dr << "package " << n << " already exists in configuration " << c <<
          info << "version: " << p->version_string ()
           << ", state: " << p->state
           << ", substate: " << p->substate;

        if (s) // Suitable state for replace?
          dr << info << "use 'pkg-checkout --replace|-r' to replace";
      }
    }

    check_any_available (rdb, t);

    // Note that here we compare including the revision (see pkg_fetch()
    // implementation for more details).
    //
    shared_ptr<available_package> ap (
      rdb.find<available_package> (package_id (n, v)));

    if (ap == nullptr)
      fail << "package " << n << " " << v << " is not available";

    // Pick a version control-based repository fragment. Preferring a local
    // one over the remotes seems like a sensible thing to do.
    //
    const package_location* pl (nullptr);

    for (const package_location& l: ap->locations)
    {
      if (!rep_masked_fragment (l.repository_fragment))
      {
        const repository_location& rl (l.repository_fragment.load ()->location);

        if (rl.version_control_based () && (pl == nullptr || rl.local ()))
        {
          pl = &l;

          if (rl.local ())
            break;
        }
      }
    }

    if (pl == nullptr)
      fail << "package " << n << " " << v
           << " is not available from a version control-based repository";

    // True if the globally cached repository needs to be used.
    //
    bool cached;

    // True if the configuration-specific repository directory exists. In this
    // case use that regardless of whether the fetch cache is enabled or not.
    // The switchover only happens on rep-fetch.
    //
    bool config_repo_exists;

    dir_path sd; // Configuration-specific repository state directory name.
    dir_path rd; // Configuration-specific repository state directory path.

    pkg_checkout_cache::state_map& cm (checkout_cache.map_);

    const repository_location& rl (pl->repository_fragment->location);
    dir_path dn (n.string () + '-' + v.string ());

    // If the fetch cache is being used and sharing of source directories is
    // not disabled, then check if the shared directory is already present in
    // the cache. If that's the case, use that. Otherwise, checkout the
    // package's commit, distribute the project, and, if required, save the
    // distribution directory into the cache.
    //
    package_id pid;
    optional<fetch_cache::loaded_shared_source_directory_state> ssd;

    if (!simulate)
    {
      sd = repository_state (rl);
      rd = rdb.config_orig / repos_dir / sd;

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

      // Check if the configuration-specific repository directory exists. Note
      // that it can already be in the checkout cache, so check there first.
      //
      config_repo_exists = (cm.find (rd) != cm.end () || exists (rd));

      cached = !config_repo_exists && fetch_cache.enabled ();

      if (cached && fetch_cache.cache_src ())
      {
        assert (fetch_cache.is_open ());

        pid = package_id (n, v);
        ssd = fetch_cache.load_shared_source_directory (pid, v);

        // Note that currently there is no scenario when the shared source
        // directory name has the form other than '<package>-<version>'.
        // Let's, however, verify that for good measure.
        //
        dir_path cdn (ssd->directory.leaf ());
        if (cdn != dn)
        {
          fail << dn << " name expected for shared source directory instead "
               << "of " << cdn <<
            info << "shared source directory: " << ssd->directory;
        }
      }
    }

    if (verb > 1 && !simulate)
    {
      text << "checking out " << pl->location.leaf () << " "
           << "from " << pl->repository_fragment->name << pdb
           << (!cached ? " (local cache)"       :
               ssd     ? " (cache, shared src)" :
                         " (cache)");
    }
    else if (((verb && !o.no_progress ()) || o.progress ()) && !simulate)
    {
      text << "checking out "
           << package_string (ap->id.name, ap->version) << pdb
           << (!cached ? " (local cache)"       :
               ssd     ? " (cache, shared src)" :
                         " (cache)");
    }
    else
      l4 ([&]{trace << pl->location.leaf () << " from "
                    << pl->repository_fragment->name << pdb;});

    auto_rmdir rmd;
    dir_path d;

    if (!simulate)
    {
      if (ssd && ssd->present)
      {
        d = move (ssd->directory);
        purge = false;
      }
      else
      {
        d = (ssd
             ? move (ssd->directory)
             : ((output_root ? *output_root : c) / dn));

        if (exists (d))
          fail << "package directory " << d << " already exists";

        // An incomplete checkout may result in an unusable repository state
        // (submodule fetch is interrupted, working tree fix up failed in the
        // middle, etc). That's why we will move the repository into the
        // temporary directory prior to manipulating it. In the case of a
        // failure (or interruption) the user will need to run bpkg-rep-fetch
        // to restore the missing repository.
        //
        assert (rl.fragment ());

        const string& commit (*rl.fragment ());

        // Use the temporary directory from the repository information source
        // configuration, so that we can always move the repository into and
        // out of it (note that if they appear on different filesystems that
        // won't be possible).
        //
        auto ti (tmp_dirs.find (rdb.config_orig));
        assert (ti != tmp_dirs.end ());
        const dir_path& tdir (ti->second);

        using state = pkg_checkout_cache::state;

        pkg_checkout_cache::state_map::iterator i;

        // NOTE: keep the subsequent checkout logic of using fetch cache and
        //       configuration-specific repository cache parallel.

        if (cached)
        {
          assert (fetch_cache.is_open ());

          repository_url url (rl.url ());
          url.fragment = nullopt;

          // Use the repository state directory in the fetch cache as a key
          // for the checkout cache. Note that it's not used for any other
          // purpose.
          //
          // Note also that we cache both local and remote URLs since a local
          // URL could be on a network filesystem or some such.
          //
          dir_path crd (fetch_cache.git_repository_state_dir (url));

          // Try to reuse the cached repository (loaded from the fetch cache
          // with some fragment checked out and fixed up).
          //
          i = cm.find (crd);

          if (i == cm.end () || i->second.rl.fragment () != rl.fragment ())
          {
            // The repository temporary directory.
            //
            // Note: only used to hold the repository path (not active).
            //
            auto_rmdir rmt;

            // Restore the repository working tree state if some different
            // fragment is checked out.
            //
            if (i != cm.end ())
            {
              rmt = checkout_cache.release (i);
            }
            else
            {
              fetch_cache::loaded_git_repository_state crs (
                fetch_cache.load_git_repository_state (move (url)));

              if (crs.state == fetch_cache::loaded_git_repository_state::absent)
              {
                // Note that this fetch cache entry won't be saved (naturally).
                //
                fail << "missing repository state for package " << n << ' '
                     << v << " in fetch cache" <<
                  info << "repository: " << rl.url () <<
                  info << "run 'bpkg rep-fetch' to repair";
              }

              rmt = auto_rmdir (move (crs.repository), false /* active */);
            }

            // Pre-insert the repository entry into the checkout cache before
            // we finalize it. This way, on failure, the repository get
            // restored in its permanent location.
            //
            i = cm.emplace (move (crd),
                            state {move (rmt),
                                   rl,
                                   true /* valid */,
                                   false /* fixedup */,
                                   &fetch_cache}).first;

            state& s (i->second);
            const dir_path& td (s.rmt.path);

            if (!git_commit_status (o, td, commit))
            {
              // Note that this fetch cache entry will be saved.
              //
              fail << "missing commit in repository state for package " << n
                   << ' ' << v << " in fetch cache" <<
                info << "repository: " << rl.url () <<
                info << "commit: " << commit <<
                info << "run 'bpkg rep-fetch' to repair";
            }

            // Checkout the repository fragment, fix up the working tree, and
            // "finalize" it by setting the valid member to true. Don't save
            // the fetch cache entry if checkout or fixup fails, unless
            // checkout fails before even starting to fetch submodules (no
            // connectivity, etc).
            //
            s.valid = false; // Make invalid not to save fetch entry on failure.

            if (!checkout (o, fetch_cache, rl, td))
            {
              s.valid = true;  // Save the fetch entry since not spoiled.
              throw failed (); // Note: the diagnostics has already been issued.
            }

            s.fixedup = *fixup (o, rl, td);
            s.valid = true;
          }
        }
        else
        {
          // Note that an existing configuration repository may contain all we
          // need.
          //
          if (fetch_cache.offline () && !config_repo_exists)
            fail << "no way to obtain state for repository " << rl.url ()
                 << " in offline mode with fetch cache disabled" <<
              info << "consider enabling fetch cache or turning offline mode off";

          // Try to reuse the cached repository (moved to the temporary
          // directory with some fragment checked out and fixed up).
          //
          i = cm.find (rd);

          if (i == cm.end () || i->second.rl.fragment () != rl.fragment ())
          {
            // The repository temporary directory.
            //
            auto_rmdir rmt;

            // Restore the repository working tree state if some different
            // fragment is checked out.
            //
            if (i != cm.end ())
            {
              rmt = checkout_cache.release (i);
            }
            else
            {
              if (!config_repo_exists)
                fail << "missing repository directory for package " << n << ' '
                     << v << " in configuration " << rdb.config_orig <<
                  info << "repository: " << rl.url () <<
                  info << "run 'bpkg rep-fetch' to repair";

              rmt = auto_rmdir (tdir / sd, !keep_tmp);

              // Move the repository to the temporary directory.
              //
              const dir_path& td (rmt.path);

              if (exists (td))
                rm_r (td);

              mv (rd, td);
            }

            // Pre-insert the repository entry into the checkout cache before
            // we finalize it. This way, on failure, the repository get
            // restored in its permanent location.
            //
            i = cm.emplace (rd,
                            state {move (rmt),
                                   rl,
                                   true /* valid */,
                                   false /* fixedup */,
                                   nullptr /* fetch_cache */ }).first;

            // Checkout the repository fragment and fix up the working tree.
            //
            state& s (i->second);
            const dir_path& td (s.rmt.path);

            if (!git_commit_status (o, td, commit))
            {
              // Note that the repository will be restored in its permanent
              // location.
              //
              fail << "missing commit in repository directory for package " << n
                   << ' ' << v << " in configuration " << rdb.config_orig <<
                info << "repository: " << rl.url () <<
                info << "commit: " << commit <<
                info << "run 'bpkg rep-fetch' to repair";
            }

            // Checkout the repository fragment, fix up the working tree, and
            // "finalize" it by setting the valid member to true. Remove the
            // repository if checkout or fixup fails, unless checkout fails
            // before even starting to fetch submodules (no connectivity,
            // etc).
            //
            s.valid = false; // Make invalid not to restore on failure.

            if (!checkout (o, fetch_cache, rl, td))
            {
              s.valid = true;  // Restore the repository since not spoiled.
              throw failed (); // Note: the diagnostics has already been issued.
            }

            s.fixedup = *fixup (o, rl, td);
            s.valid = true;
          }
        }

        // The temporary out of source directory that is required for the dist
        // meta-operation.
        //
        auto_rmdir rmo (tdir / dir_path (n.string ()), !keep_tmp);
        const dir_path& od (rmo.path);

        if (exists (od))
          rm_r (od);

        // Calculate the package path that points into the checked out
        // fragment directory.
        //
        dir_path pd (i->second.rmt.path / path_cast<dir_path> (pl->location));

        // Form the buildspec.
        //
        string bspec ("dist('");
        bspec += pd.representation ();
        bspec += "'@'";
        bspec += od.representation ();
        bspec += "')";

        // Remove the resulting package distribution directory on failure.
        //
        rmd = auto_rmdir (d);

        // Distribute.
        //
        // Note that we are using the bootstrap distribution mode (and also
        // skip bootstrapping external modules) to make sure a package can be
        // checked out without its dependencies being present.
        //
        // Note also that on failure the package stays in the existing
        // (working) state.
        //
        // At first it may seem we have a problem: an existing package with
        // the same name will cause a conflict since we now have multiple
        // package locations for the same package name. We are lucky, however:
        // subprojects are only loaded if used and since we don't support
        // dependency cycles, the existing project should never be loaded by
        // any of our dependencies.
        //

        // If the verbosity level is less than 2, then we want our (nicer)
        // progress header but the build system's actual progress.
        //
        if ((verb == 1 && !o.no_progress ()) || (verb == 0 && o.progress ()))
          text << "distributing " << n << '/' << v << pdb;

        run_b (o,
               verb_b::progress,
               false /* no_progress */,
               "--no-external-modules",
               "!config.dist.bootstrap=true",
               "config.dist.root='" + d.directory ().representation () + '\'',
               bspec);

        if (ssd)
        {
          d = fetch_cache.save_shared_source_directory (move (pid),
                                                        v,
                                                        move (d),
                                                        rl.url (),
                                                        commit);
          purge = false;
        }
      }
    }
    else
      d = (output_root ? *output_root : c) / dn;

    if (p != nullptr)
    {
      // Clean up the source directory and archive of the package we are
      // replacing. Once this is done, there is no going back. If things go
      // badly, we can't simply abort the transaction.
      //
      pkg_purge_fs (pdb, t, p, simulate);

      // Note that if the package name spelling changed then we need to update
      // it, to make sure that the subsequent commands don't fail and the
      // diagnostics is not confusing. Hover, we cannot update the object id,
      // so have to erase it and persist afterwards.
      //
      if (p->name.string () != n.string ())
      {
        pdb.erase (p);
        p = nullptr;
      }
    }

    // Make the package path absolute and normalized. If the package is inside
    // the configuration, use the relative path. This way we can move the
    // configuration around.
    //
    normalize (d, "package");

    if (d.sub (pdb.config))
      d = d.leaf (pdb.config);

    // Make sure all the available package sections, required for generating
    // the manifest, are loaded.
    //
    if (!ap->languages_section.loaded ())
      rdb.load (*ap, ap->languages_section);

    if (p != nullptr)
    {
      // Note: we can be replacing an external package and thus we reset the
      // manifest/subprojects and buildfiles checksums.
      //
      p->version = move (v);
      p->state = package_state::unpacked;
      p->repository_fragment = rl;
      p->src_root = move (d);
      p->purge_src = purge;
      p->manifest_checksum = nullopt;
      p->buildfiles_checksum = nullopt;
      p->manifest = ap->manifest ();

      // Mark the section as loaded, so the manifest is updated.
      //
      p->manifest_section.load ();

      pdb.update (p);
    }
    else
    {
      // Add the package to the configuration.
      //
      p.reset (new selected_package {
        move (n),
        move (v),
        package_state::unpacked,
        package_substate::none,
        false,     // hold package
        false,     // hold version
        rl,
        nullopt,   // No archive
        false,
        move (d),  // Source root.
        purge,     // Purge directory.
        nullopt,   // No manifest/subprojects checksum.
        nullopt,   // No buildfiles checksum.
        nullopt,   // No output directory yet.
        {},        // No prerequisites captured yet.
        ap->manifest ()});

      pdb.persist (p);
    }

    t.commit ();

    rmd.cancel ();
    return p;
  }

  shared_ptr<selected_package>
  pkg_checkout (const common_options& o,
                bpkg::fetch_cache& fetch_cache,
                pkg_checkout_cache& checkout_cache,
                database& pdb,
                database& rdb,
                transaction& t,
                package_name n,
                version v,
                const dir_path& d,
                bool replace,
                bool purge,
                bool simulate)
  {
    return pkg_checkout (o,
                         fetch_cache,
                         checkout_cache,
                         pdb,
                         rdb,
                         t,
                         move (n),
                         move (v),
                         optional<dir_path> (d),
                         replace,
                         purge,
                         simulate);
  }

  shared_ptr<selected_package>
  pkg_checkout (const common_options& o,
                bpkg::fetch_cache& fetch_cache,
                pkg_checkout_cache& checkout_cache,
                database& pdb,
                database& rdb,
                transaction& t,
                package_name n,
                version v,
                bool replace,
                bool simulate)
  {
    return pkg_checkout (o,
                         fetch_cache,
                         checkout_cache,
                         pdb,
                         rdb,
                         t,
                         move (n),
                         move (v),
                         nullopt /* output_root */,
                         replace,
                         true /* purge */,
                         simulate);
  }

  int
  pkg_checkout (const pkg_checkout_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_checkout");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    database db (c,
                 o.sqlite_synchronous (),
                 trace,
                 true /* pre_attach */,
                 false /* sys_rep */);

    transaction t (db);
    session s;

    shared_ptr<selected_package> p;

    if (!args.more ())
      fail << "package name/version argument expected" <<
        info << "run 'bpkg help pkg-checkout' for more information";

    const char*  arg (args.next ());
    package_name n   (parse_package_name (arg));
    version      v   (parse_package_version (arg));

    if (v.empty ())
      fail << "package version expected" <<
        info << "run 'bpkg help pkg-checkout' for more information";

    bpkg::fetch_cache fetch_cache (o, &db);

    if (fetch_cache.enabled ())
      fetch_cache.open (trace);

    pkg_checkout_cache checkout_cache (o);

    // Commits the transaction.
    //
    if (o.output_root_specified ())
      p = pkg_checkout (o,
                        fetch_cache,
                        checkout_cache,
                        db /* pdb */,
                        db /* rdb */,
                        t,
                        move (n),
                        move (v),
                        o.output_root (),
                        o.replace (),
                        o.output_purge (),
                        false /* simulate */);
    else
      p = pkg_checkout (o,
                        fetch_cache,
                        checkout_cache,
                        db /* pdb */,
                        db /* rdb */,
                        t,
                        move (n),
                        move (v),
                        o.replace (),
                        false /* simulate */);

    checkout_cache.clear (); // Detect errors.

    if (fetch_cache.enabled ())
      fetch_cache.close ();

    if (verb && !o.no_result ())
      text << "checked out " << *p;

    return 0;
  }

  // pkg_checkout_cache
  //
  pkg_checkout_cache::
  ~pkg_checkout_cache ()
  {
    if (!map_.empty () && !clear (false /* fail */))
    {
      // We assume that the diagnostics has already been issued.
      //
      warn << "repository state is now broken" <<
        info << "run 'bpkg rep-fetch' to repair";
    }
  }

  bool pkg_checkout_cache::
  clear (bool fail)
  {
    // Let's always return to the permanent location as many repositories as
    // possible.
    //
    bool r (true);

    using iterator = pkg_checkout_cache::state_map::iterator;

    for (iterator i (map_.begin ()); i != map_.end (); )
    {
      iterator j (i++);

      if (!erase (j, false /* fail */))
        r = false;
    }

    if (!r && fail)
      throw failed (); // The diagnostics is already issued.

    return r;
  }

  bool pkg_checkout_cache::
  erase (state_map::iterator i, bool fail)
  {
    state& s (i->second);

    // Auto-removal should never be active for the fetch cache entries.
    //
    assert (s.fetch_cache == nullptr || !s.rmt.active);

    // Bail out if the entry is invalid.
    //
    if (!s.valid)
    {
      assert (!fail); // Only makes sense if we don't fail on errors.
      return false;
    }

    // Remove the working tree and return the repository to the permanent
    // location.
    //
    // But first make the entry invalid, so on error we don't try to restore
    // the partially restored repository later.
    //
    s.valid = false;

    const dir_path& td (s.rmt.path);

    if (!git_remove_worktree (options_, td, fail))
      return false;

    // Manipulations over the repository are now complete, so we can return it
    // to the permanent location.
    //
    if (s.fetch_cache != nullptr)
    {
      try
      {
        repository_url url (s.rl.url ());
        url.fragment = nullopt;

        s.fetch_cache->save_git_repository_state (move (url));
      }
      catch (const failed&)
      {
        if (fail)
          throw;

        return false;
      }
    }
    else
    {
      if (!mv (td, i->first, fail))
        return false;

      s.rmt.cancel ();
    }

    map_.erase (i);
    return true;
  }

  auto_rmdir pkg_checkout_cache::
  release (state_map::iterator i, bool fail)
  {
    state& s (i->second);

    // Auto-removal should never be active for the fetch cache entries.
    //
    assert (s.fetch_cache == nullptr || !s.rmt.active);

    // Bail out if the entry is invalid.
    //
    if (!s.valid)
    {
      assert (!fail);       // Only makes sense if we don't fail on errors.
      return auto_rmdir ();
    }

    // Revert the fix-ups.
    //
    // But first make the entry invalid, so on error we don't try to restore
    // the partially restored repository later.
    //
    s.valid = false;

    if (s.fixedup &&
        !fixup (options_, s.rl, s.rmt.path, true /* revert */, fail))
      return auto_rmdir ();

    auto_rmdir r (move (s.rmt));
    map_.erase (i);
    return r;
  }
}
