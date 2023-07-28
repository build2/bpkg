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
#include <bpkg/manifest-utility.hxx>

#include <bpkg/pkg-purge.hxx>
#include <bpkg/pkg-verify.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  // pkg_checkout()
  //
  static void
  checkout (const common_options& o,
            const repository_location& rl,
            const dir_path& dir,
            const shared_ptr<available_package>& ap,
            database& db)
  {
    switch (rl.type ())
    {
    case repository_type::git:
      {
        assert (rl.fragment ());

        git_checkout (o, dir, *rl.fragment ());

        if (exists (dir / path (".gitmodules")))
        {
          // Print the progress indicator to attribute the possible fetching
          // progress.
          //
          if ((verb && !o.no_progress ()) || o.progress ())
            text << "checking out "
                 << package_string (ap->id.name, ap->version) << db;

          git_checkout_submodules (o, rl, dir);
        }

        break;
      }
    case repository_type::pkg:
    case repository_type::dir: assert (false); break;
    }
  }

  // For some platforms/repository types the working tree needs to be
  // temporary "fixed up" for the build2 operations to work properly on it.
  //
  static optional<bool>
  fixup (const common_options& o,
         const repository_location& rl,
         const dir_path& dir,
         bool revert = false,
         bool ie = false)
  {
    optional<bool> r;

    switch (rl.type ())
    {
    case repository_type::git:
      {
        if (!revert && !ie)
          git_verify_symlinks (o, dir);

        r = git_fixup_worktree (o, dir, revert, ie);
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
  pkg_checkout (pkg_checkout_cache& cache,
                const common_options& o,
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
      rdb.find<available_package> (available_package_id (n, v)));

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

    if (verb > 1)
      text << "checking out " << pl->location.leaf () << " "
           << "from " << pl->repository_fragment->name;

    const repository_location& rl (pl->repository_fragment->location);

    auto_rmdir rmd;
    const dir_path& ord (output_root ? *output_root : c);
    dir_path d (ord / dir_path (n.string () + '-' + v.string ()));

    // An incomplete checkout may result in an unusable repository state
    // (submodule fetch is interrupted, working tree fix up failed in the
    // middle, etc.). That's why we will move the repository into the
    // temporary directory prior to manipulating it. In the case of a failure
    // (or interruption) the user will need to run bpkg-rep-fetch to restore
    // the missing repository.
    //
    if (!simulate)
    {
      if (exists (d))
        fail << "package directory " << d << " already exists";

      // Check that the repository directory exists, which may not be the case
      // if the previous checkout have failed or been interrupted.
      //
      dir_path sd (repository_state (rl));
      dir_path rd (rdb.config_orig / repos_dir / sd);

      // Use the temporary directory from the repository information source
      // configuration, so that we can always move the repository into and out
      // of it (note that if they appear on different filesystems that won't
      // be possible).
      //
      auto ti (tmp_dirs.find (rdb.config_orig));
      assert (ti != tmp_dirs.end ());
      const dir_path& tdir (ti->second);

      // Try to reuse the cached repository (moved to the temporary directory
      // with some fragment checked out and fixed up).
      //
      pkg_checkout_cache::state_map& cm (cache.map_);
      auto i (cm.find (rd));

      if (i == cm.end () || i->second.rl.fragment () != rl.fragment ())
      {
        // Restore the repository if some different fragment is checked out.
        //
        if (i != cm.end ())
          cache.erase (i);

        // Checkout and cache the fragment.
        //
        if (!exists (rd))
          fail << "missing repository directory for package " << n << " " << v
               << " in its repository information configuration "
               << rdb.config_orig <<
            info << "run 'bpkg rep-fetch' to repair";

        // The repository temporary directory.
        //
        auto_rmdir rmt (tdir / sd, !keep_tmp);

        // Move the repository to the temporary directory.
        //
        {
          const dir_path& td (rmt.path);

          if (exists (td))
            rm_r (td);

          mv (rd, td);
        }

        // Pre-insert the incomplete repository entry into the cache and
        // "finalize" it by setting the fixed up value later, after the
        // repository fragment checkout succeeds. Until then the repository
        // may not be restored in its permanent place.
        //
        using state = pkg_checkout_cache::state;

        i = cm.emplace (rd, state {move (rmt), rl, nullopt}).first;

        // Checkout the repository fragment and fix up the working tree.
        //
        state& s (i->second);
        const dir_path& td (s.rmt.path);

        checkout (o, rl, td, ap, pdb);
        s.fixedup = fixup (o, rl, td);
      }

      // The temporary out of source directory that is required for the dist
      // meta-operation.
      //
      auto_rmdir rmo (tdir / dir_path (n.string ()), !keep_tmp);
      const dir_path& od (rmo.path);

      if (exists (od))
        rm_r (od);

      // Calculate the package path that points into the checked out fragment
      // directory.
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
      // Note that we are using the bootstrap distribution mode (and also skip
      // bootstrapping external modules) to make sure a package can be checked
      // out without its dependencies being present.
      //
      // Note also that on failure the package stays in the existing (working)
      // state.
      //
      // At first it may seem we have a problem: an existing package with the
      // same name will cause a conflict since we now have multiple package
      // locations for the same package name. We are lucky, however:
      // subprojects are only loaded if used and since we don't support
      // dependency cycles, the existing project should never be loaded by any
      // of our dependencies.
      //

      // If the verbosity level is less than 2, then we want our (nicer)
      // progress header but the build system's actual progress.
      //
      if ((verb == 1 && !o.no_progress ()) || (verb == 0 && o.progress ()))
        text << "distributing " << n << '/' << v << pdb;

      run_b (o,
             verb_b::progress,
             "--no-external-modules",
             "!config.dist.bootstrap=true",
             "config.dist.root='" + ord.representation () + '\'',
             bspec);
    }

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
        {}});      // No prerequisites captured yet.

      pdb.persist (p);
    }

    t.commit ();

    rmd.cancel ();
    return p;
  }

  shared_ptr<selected_package>
  pkg_checkout (pkg_checkout_cache& cache,
                const common_options& o,
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
    return pkg_checkout (cache,
                         o,
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
  pkg_checkout (pkg_checkout_cache& cache,
                const common_options& o,
                database& pdb,
                database& rdb,
                transaction& t,
                package_name n,
                version v,
                bool replace,
                bool simulate)
  {
    return pkg_checkout (cache,
                         o,
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

    database db (c, trace, true /* pre_attach */);
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

    pkg_checkout_cache checkout_cache (o);

    // Commits the transaction.
    //
    if (o.output_root_specified ())
      p = pkg_checkout (checkout_cache,
                        o,
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
      p = pkg_checkout (checkout_cache,
                        o,
                        db /* pdb */,
                        db /* rdb */,
                        t,
                        move (n),
                        move (v),
                        o.replace (),
                        false /* simulate */);

    checkout_cache.clear (); // Detect errors.

    if (verb && !o.no_result ())
      text << "checked out " << *p;

    return 0;
  }

  // pkg_checkout_cache
  //
  pkg_checkout_cache::
  ~pkg_checkout_cache ()
  {
    if (!map_.empty () && !clear (true /* ignore_errors */))
    {
      // We assume that the diagnostics has already been issued.
      //
      warn << "repository state is now broken" <<
        info << "run 'bpkg rep-fetch' to repair";
    }
  }

  bool pkg_checkout_cache::
  clear (bool ie)
  {
    while (!map_.empty ())
    {
      if (!erase (map_.begin (), ie))
        return false;
    }

    return true;
  }

  bool pkg_checkout_cache::
  erase (state_map::iterator i, bool ie)
  {
    state& s (i->second);

    // Bail out if the entry is incomplete.
    //
    if (!s.fixedup)
    {
      assert (ie); // Only makes sense in the ignore errors mode.
      return false;
    }

    // Revert the fix-ups.
    //
    // But first make the entry incomplete, so on error we don't try to
    // restore the partially restored repository later.
    //
    bool f (*s.fixedup);

    s.fixedup = nullopt;

    if (f && !fixup (options_, s.rl, s.rmt.path, true /* revert */, ie))
      return false;

    // Manipulations over the repository are now complete, so we can return it
    // to the permanent location.
    //
    if (!mv (s.rmt.path, i->first, ie))
      return false;

    s.rmt.cancel ();

    map_.erase (i);
    return true;
  }
}
