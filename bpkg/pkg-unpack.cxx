// file      : bpkg/pkg-unpack.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-unpack.hxx>

#include <libbpkg/manifest.hxx>

#include <bpkg/archive.hxx>
#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/checksum.hxx>
#include <bpkg/rep-mask.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/fetch-cache.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/pkg-purge.hxx>
#include <bpkg/pkg-fetch.hxx>
#include <bpkg/pkg-verify.hxx>

using namespace std;

namespace bpkg
{
  // Check if the package already exists in this configuration and
  // diagnose all the illegal cases.
  //
  static void
  pkg_unpack_check (database& db,
                    transaction&,
                    const package_name& n,
                    bool replace)
  {
    tracer trace ("pkg_update_check");

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
          dr << info << "use 'pkg-unpack --replace|-r' to replace";
      }
    }
  }

  // Select the external package in this configuration. Return the selected
  // package object which may replace the existing one.
  //
  static shared_ptr<selected_package>
  pkg_unpack (database& db,
              transaction& t,
              package_name&& n,
              version&& v,
              dir_path&& d,
              repository_location&& rl,
              shared_ptr<selected_package>&& p,
              optional<string>&& mc,
              optional<string>&& bc,
              string&& m,
              bool purge,
              bool simulate)
  {
    // Make the package path absolute and normalized. If the package is inside
    // the configuration, use the relative path. This way we can move the
    // configuration around.
    //
    normalize (d, "package");

    if (d.sub (db.config))
      d = d.leaf (db.config);

    if (p != nullptr)
    {
      // Clean up the source directory and archive of the package we are
      // replacing. Once this is done, there is no going back. If things
      // go badly, we can't simply abort the transaction.
      //
      pkg_purge_fs (db, t, p, simulate);

      // Note that if the package name spelling changed then we need to update
      // it, to make sure that the subsequent commands don't fail and the
      // diagnostics is not confusing. Hover, we cannot update the object id,
      // so have to erase it and persist afterwards.
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
      p->state = package_state::unpacked;
      p->repository_fragment = move (rl);
      p->src_root = move (d);
      p->purge_src = purge;
      p->manifest_checksum = move (mc);
      p->buildfiles_checksum = move (bc);
      p->manifest = move (m);

      // Mark the section as loaded, so the manifest is updated.
      //
      p->manifest_section.load ();

      db.update (p);
    }
    else
    {
      p.reset (new selected_package {
        move (n),
        move (v),
        package_state::unpacked,
        package_substate::none,
        false,      // hold package
        false,      // hold version
        move (rl),
        nullopt,    // No archive
        false,      // Don't purge archive.
        move (d),
        purge,
        move (mc),
        move (bc),
        nullopt,    // No output directory yet.
        {},         // No prerequisites captured yet.
        move (m)});

      db.persist (p);
    }

    assert (p->external ());

    t.commit ();
    return move (p);
  }

  static shared_ptr<selected_package>
  pkg_unpack (const common_options& o,
              database& db,
              transaction& t,
              package_name n,
              version v,
              const dependencies& deps,
              const package_info* pi,
              dir_path d,
              repository_location rl,
              string m,
              bool purge,
              bool simulate)
  {
    tracer trace ("pkg_unpack");

    tracer_guard tg (db, trace);

    shared_ptr<selected_package> p (db.find<selected_package> (n));

    optional<string> mc;
    optional<string> bc;

    // Only calculate the manifest/subprojects and buildfiles checksums for
    // external packages (see selected_package::external() for details).
    //
    if (!simulate && (rl.empty () || rl.directory_based ()))
    {
      mc = package_checksum (o, d, pi);

      // Calculate the buildfiles checksum if the package has any buildfile
      // clauses in the dependencies. Always calculate it over the buildfiles
      // since the package is external.
      //
      if ((p != nullptr && p->manifest_checksum == mc)
          ? p->buildfiles_checksum.has_value ()
          : has_buildfile_clause (deps))
        bc = package_buildfiles_checksum (nullopt /* bootstrap_build */,
                                          nullopt /* root_build */,
                                          {}      /* buildfiles */,
                                          d);
    }

    return pkg_unpack (db,
                       t,
                       move (n),
                       move (v),
                       move (d),
                       move (rl),
                       move (p),
                       move (mc),
                       move (bc),
                       move (m),
                       purge,
                       simulate);
  }

  static shared_ptr<selected_package>
  pkg_switch (const common_options& o,
              database& db,
              transaction& t,
              path a,
              bool purge)
  {
    tracer trace ("pkg_switch");

    if (!exists (a))
      fail << "archive file '" << a << "' does not exist";

    // Verify archive is a package (contains <name>-<version> subdirectory,
    // etc) and get its manifest.
    //
    package_manifest m (pkg_verify (o,
                                    a,
                                    true /* ignore_unknown */,
                                    false /* ignore_toolchain */,
                                    false /* expand_values */,
                                    false /* load_buildfiles */));

    l4 ([&]{trace << a << ": " << m.name << " " << m.version;});

    tracer_guard tg (db, trace);

    // Verify that the package is selected, is configured out of source, is
    // not external, and is of the same version as in the archive.
    //
    // Note that in the future, we may remove some of these restrictions.
    //
    shared_ptr<selected_package> p (db.find<selected_package> (m.name));

    if (p == nullptr)
      fail << "package " << m.name << " does not exist in configuration "
           << db.config_orig;

    if (p->state != package_state::configured)
      fail << "unable to switch source directory for " << p->state
           << " package " << p->string (db);

    if (p->system ())
      fail << "unable to switch source directory for system package "
           << p->string (db);

    assert (p->src_root); // Must be set since unpacked.
    assert (p->out_root); // Must be set since configured.

    dir_path src_root (p->effective_src_root (db.config_orig));
    dir_path out_root (p->effective_out_root (db.config_orig));

    l4 ([&]{trace << "src_root: " << src_root << ", "
                  << "out_root: " << out_root;});

    if (src_root == out_root)
      fail << p->string (db) << " is already configured in source";

    if (p->external ())
      fail << "unable to switch source directory for external package "
           << p->string (db);

    if (p->version != m.version)
      fail << "unable to switch source directory for package "
           << p->string (db) << " since archive contains different version ("
           << m.version << ")";

    // The overall plan is as follows:
    //
    // - Setup auto-removal for the output directory, so that it is removed
    //   if spoiled.
    //
    // - Create the make_exception_guard object which, on exception, sets the
    //   broken state for the package object and issues the respective info.
    //
    // - Remove the build[2]/bootstrap/ subdirectory for the package and its
    //   sub-projects from the package output directory.
    //
    // - Purge the original source directory and archive, if requested.
    //
    // - Extract the archive contents into the output directory.
    //
    // - Change the package database object and persist the change.
    //
    // - Commit the transaction.
    //
    // - Cancel the output directory auto removal.
    //
    dir_path dn (p->name.string () + '-' + p->version.string ());

    assert (dn == out_root.leaf ()); // Since the package is not external.

    package_info pi (package_b_info (o, out_root, b_info_flags::subprojects));

    auto_rmdir arm (out_root);

    auto g (
      make_exception_guard (
        [&p, &db, &t] ()
        {
          p->state = package_state::broken;
          db.update (p);
          t.commit ();

          // Here we assume that the diagnostics is already issued.
          //
          info << "package " << p->name << db << " is now broken; "
               << "use 'pkg-purge --force' to remove";
        }));

    // Remove build[2]/bootstrap/ subdirectories.
    //
    auto rm_bootstrap = [] (const dir_path& out_root)
    {
      dir_path d (exists (out_root / alt_build_dir) // Alternative naming?
                  ? out_root / alt_bootstrap_dir
                  : out_root / std_bootstrap_dir);

      if (exists (d)) // Don't complain if someone did our job for us.
        rm_r (d);
    };

    rm_bootstrap (out_root);

    for (const package_info::subproject& sp: pi.subprojects)
      rm_bootstrap (out_root / sp.path);

    // Purge the original source directory and archive.
    //
    // Let's do this prior to extracting the archive contents to free up a bit
    // of space. Keep in mind that we may still end up with the broken package
    // state, so keep the object data consistent with the file system at every
    // step of the switch.
    //
    if (p->purge_src)
    {
      if (exists (src_root)) // Don't complain if someone did our job for us.
        rm_r (src_root);

      p->purge_src = false;
    }

    p->src_root = nullopt;

    if (p->purge_archive)
    {
      path a (p->effective_archive (db.config_orig));

      if (exists (a))
        rm (a);

      p->purge_archive = false;
    }

    p->archive = nullopt;

    // Extract the archive contents.
    //
    if (verb > 1)
      text << "unpacking " << dn << " from " << a << db;
    else if ((verb && !o.no_progress ()) || o.progress ())
      text << "unpacking " << *p << db;
    else
      l4 ([&]{trace << dn << " from " << a << db;});

    dir_path pd (out_root.directory ());

    try
    {
      pair<process, process> pr (start_extract (o, a, pd));

      // While it is reasonable to assuming the child process issued
      // diagnostics, tar, specifically, doesn't mention the archive name.
      //
      if (!pr.second.wait () || !pr.first.wait ())
        fail << "unable to extract " << a << " to " << pd;
    }
    catch (const process_error& e)
    {
      fail << "unable to extract " << a << " to " << pd << ": " << e;
    }

    // Update the package object.
    //
    p->src_root = p->out_root;
    p->purge_src = true;

    // Make the archive path absolute and normalized. If the archive is inside
    // the configuration, use the relative path. This way we can move the
    // configuration around.
    //
    normalize (a, "archive");

    if (a.sub (db.config))
      a = a.leaf (db.config);

    p->archive = move (a);
    p->purge_archive = purge;
    p->repository_fragment = repository_location ();

    db.update (p);
    t.commit ();

    arm.cancel (); // Cannot throw.

    return p;
  }

  static shared_ptr<selected_package>
  pkg_unpack (const common_options& o,
              database& db,
              transaction& t,
              path a,
              bool replace,
              bool purge)
  {
    tracer trace ("pkg_unpack");

    l4 ([&]{trace << "archive: " << a << db << ", purge: " << purge;});

    shared_ptr<selected_package> p (
      pkg_fetch (o,
                 db,
                 t,
                 move (a),
                 replace,
                 purge,
                 false /* simulate */,
                 true /* keep_transaction_if_safe */));

    if (t.finalized ())
      t.start (db);

    // Note that since the package archive is used in place, there is no sence
    // to open the fetch cache (see pkg_unpack() for details).
    //
    assert (p->repository_fragment.empty ());

    fetch_cache cache (o, &db);

    p = pkg_unpack (o,
                    cache,
                    db,
                    t,
                    p->name,
                    false /* simulate */);

    return p;
  }

  shared_ptr<selected_package>
  pkg_unpack (const common_options& o,
              database& db,
              transaction& t,
              const dir_path& d,
              bool replace,
              bool purge,
              bool simulate)
  {
    tracer trace ("pkg_unpack");

    if (!exists (d))
      fail << "package directory " << d << " does not exist";

    // For better diagnostics, let's obtain the package info after
    // pkg_verify() verifies that this is a package directory.
    //
    package_version_info pvi;

    // Verify the directory is a package and get its manifest.
    //
    package_manifest m (
      pkg_verify (o,
                  d,
                  true /* ignore_unknown */,
                  false /* ignore_toolchain */,
                  true /* load_buildfiles */,
                  [&o, &d, &pvi] (version& v)
                  {
                    // Note that we also query subprojects since the package
                    // information will be used for the subsequent
                    // package_iteration() call.
                    //
                    pvi = package_version (o, d, b_info_flags::subprojects);

                    if (pvi.version)
                      v = move (*pvi.version);
                  }));

    l4 ([&]{trace << d << ": " << m.name << " " << m.version;});

    // Check/diagnose an already existing package.
    //
    pkg_unpack_check (db, t, m.name, replace);

    // Fix-up the package version.
    //
    if (optional<version> v = package_iteration (o,
                                                 db,
                                                 t,
                                                 d,
                                                 m.name,
                                                 m.version,
                                                 &pvi.info,
                                                 true /* check_external */))
      m.version = move (*v);

    // Create the temporary available package object from the package manifest
    // to serialize it into the available package manifest string.
    //
    available_package ap (move (m));
    string s (ap.manifest ());

    // Use the special root repository fragment as the repository fragment of
    // this package.
    //
    return pkg_unpack (o,
                       db,
                       t,
                       move (ap.id.name),
                       move (ap.version),
                       ap.dependencies,
                       &pvi.info,
                       d,
                       repository_location (),
                       move (s),
                       purge,
                       simulate);
  }

  shared_ptr<selected_package>
  pkg_unpack (const common_options& o,
              database& pdb,
              database& rdb,
              transaction& t,
              package_name n,
              version v,
              bool replace,
              bool simulate)
  {
    tracer trace ("pkg_unpack");

    tracer_guard tg (pdb, trace); // NOTE: sets tracer for the whole cluster.

    // Check/diagnose an already existing package.
    //
    pkg_unpack_check (pdb, t, n, replace);

    check_any_available (rdb, t);

    // Note that here we compare including the revision (see pkg_fetch()
    // implementation for more details).
    //
    shared_ptr<available_package> ap (
      rdb.find<available_package> (package_id (n, v)));

    if (ap == nullptr)
      fail << "package " << n << " " << v << " is not available";

    // Pick a directory-based repository fragment. They are always local, so we
    // pick the first one.
    //
    const package_location* pl (nullptr);

    for (const package_location& l: ap->locations)
    {
      if (!rep_masked_fragment (l.repository_fragment) &&
          l.repository_fragment.load ()->location.directory_based ())
      {
        pl = &l;
        break;
      }
    }

    if (pl == nullptr)
      fail << "package " << n << " " << v
           << " is not available from a directory-based repository";

    // Note: we currently don't print verb=1 progress here since there is no
    // cache involved and it would spoil bdep diagnostics.
    //
    if (verb > 1 && !simulate)
      text << "unpacking " << pl->location.leaf () << " "
           << "from " << pl->repository_fragment->name << pdb;
    else
      l4 ([&]{trace << pl->location.leaf () << " from "
                    << pl->repository_fragment->name << pdb;});

    const repository_location& rl (pl->repository_fragment->location);

    // Make sure all the available package sections, required for generating
    // the manifest, are loaded.
    //
    if (!ap->languages_section.loaded ())
      rdb.load (*ap, ap->languages_section);

    return pkg_unpack (o,
                       pdb,
                       t,
                       move (n),
                       move (v),
                       ap->dependencies,
                       nullptr   /* package_info */,
                       path_cast<dir_path> (rl.path () / pl->location),
                       rl,
                       ap->manifest (),
                       false     /* purge */,
                       simulate);
  }

  shared_ptr<selected_package>
  pkg_unpack (const common_options& co,
              fetch_cache& cache,
              database& db,
              transaction& t,
              const package_name& name,
              bool simulate,
              bool omit_progress)
  {
    tracer trace ("pkg_unpack");

    tracer_guard tg (db, trace);

    const dir_path& c (db.config_orig);
    shared_ptr<selected_package> p (db.find<selected_package> (name));

    if (p == nullptr)
      fail << "package " << name << " does not exist in configuration " << c;

    if (p->state != package_state::fetched)
      fail << "package " << name << db << " is " << p->state <<
        info << "expected it to be fetched";

    l4 ([&]{trace << *p;});

    assert (p->archive); // Should have archive in the fetched state.

    // If the package archive is not used in place, the fetch cache is
    // enabled, and sharing of source directories is not disabled, then check
    // if the shared directory is already present in the cache. If that's the
    // case, use that. Otherwise, extract the package directory and, if
    // required, save it into the cache.
    //
    // Also, in the latter case, since we must have verified the archive
    // during fetch, here we can just assume what the resulting directory will
    // be.
    //
    package_id pid;
    optional<fetch_cache::loaded_shared_source_directory_state> ssd;

    const package_name& n (p->name);
    const version& v (p->version);

    dir_path dn (n.string () + '-' + v.string ());
    const repository_location& rl (p->repository_fragment);

    if (!simulate)
    {
      // Note: see also complementary shared src logic in pkg-fetch. Note that
      // it's possible to craft a scenario where we will unpack an archive
      // that doesn't come from the fetch cache. This, however, seems harmless
      // and so we don't check.
      //
      if (!rl.empty () && cache.cache_src ())
      {
        assert (cache.is_open ());

        pid = package_id (n, v);
        ssd = cache.load_shared_source_directory (pid, v);

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
      text << "unpacking " << dn << " from " << p->effective_archive (c) << db
           << (ssd ? " (cache, shared src)" : "");
    }
    else if (((verb && !co.no_progress ()) || co.progress ()) && !simulate)
    {
      if (!omit_progress)
        text << "unpacking " << *p << db << (ssd ? " (cache, shared src)" : "");
    }
    else
      l4 ([&]{trace << dn << " from " << p->effective_archive (c) << db;});

    auto_rmdir arm;

    if (!simulate)
    {
      if (ssd && ssd->present)
      {
        // Make the source directory path absolute and normalized.
        //
        p->src_root = move (normalize (ssd->directory,
                                       "shared source directory"));

        p->purge_src = false;
      }
      else
      {
        dir_path d (ssd ? move (ssd->directory) : c / dn);

        if (exists (d))
          fail << "package directory " << d << " already exists";

        // If the archive path is not absolute, then it must be relative
        // to the configuration.
        //
        path a (p->effective_archive (c));

        l4 ([&]{trace << "archive: " << a;});

        // What should we do if tar or something after it fails? Cleaning
        // up the package directory sounds like the right thing to do.
        //
        arm = auto_rmdir (d);

        dir_path pd (d.directory ());

        try
        {
          pair<process, process> pr (start_extract (co, a, pd));

          // While it is reasonable to assuming the child process issued
          // diagnostics, tar, specifically, doesn't mention the archive name.
          //
          if (!pr.second.wait () || !pr.first.wait ())
            fail << "unable to extract " << a << " to " << pd;
        }
        catch (const process_error& e)
        {
          fail << "unable to extract " << a << " to " << pd << ": " << e;
        }

        if (!exists (d))
          fail << "package archive " << a << " doesn't contain directory "
               << dn;

        if (ssd)
        {
          // Note that the archive file checksum, as it comes from
          // packages.manifest file, is not available at this point. Thus, we
          // just recalculate it.
          //
          d = cache.save_shared_source_directory (move (pid),
                                                  v,
                                                  move (d),
                                                  rl.url (),
                                                  sha256sum (co, a));

          // Make the source directory path absolute and normalized.
          //
          p->src_root = move (normalize (d, "shared source directory"));

          p->purge_src = false;
        }
        else
        {
          p->src_root = move (dn);
          p->purge_src = true;
        }
      }
    }
    else
    {
      p->src_root = move (dn); // For now assuming to be in configuration.
      p->purge_src = true;
    }

    p->state = package_state::unpacked;

    db.update (p);
    t.commit ();

    arm.cancel ();

    return p;
  }

  int
  pkg_unpack (const pkg_unpack_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_unpack");

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    database db (c,
                 o.sqlite_synchronous (),
                 trace,
                 true /* pre_attach */,
                 false /* sys_rep */);

    transaction t (db);

    shared_ptr<selected_package> p;
    bool external;

    if (o.existing ())
    {
      // The package directory case.
      //
      if (!args.more ())
        fail << "package archive or directory argument expected" <<
          info << "run 'bpkg help pkg-unpack' for more information";

      try
      {
        path pp (args.next ());

        if (pp.empty ())
          throw invalid_path ("");

        if (pp.to_directory ())
        {
          if (o.switch_ ())
            fail << "existing archive must be specified with --switch" <<
              info << "run 'bpkg help pkg-unpack' for more information";

          p = pkg_unpack (o,
                          db,
                          t,
                          path_cast<dir_path> (move (pp)),
                          o.replace (),
                          o.purge (),
                          false /* simulate */);

          external = true;
        }
        else
        {
          // Note that previously, before we added support for unpacking
          // existing package archives, we didn't require the trailing slash
          // for the package directory. Thus, let's help the user out here a
          // bit, if path, specified without the trailing slash, is an
          // existing directory.
          //
          if (exists (path_cast<dir_path> (pp)))
          {
            char c (dir_path::traits_type::directory_separator); // GCC 4.9.

            fail << "path '" << pp << "' refers to existing directory but not "
                 << "to package archive" <<
              info << "consider specifying trailing '" << c << "' if this is "
                   << "a package directory" <<
              info << "run 'bpkg help pkg-unpack' for more information";
          }

          if (o.switch_ ())
          {
            if (o.replace ())
              fail << "both --switch and --replace|-r specified";

            p = pkg_switch (o, db, t, move (pp), o.purge ());
          }
          else
            p = pkg_unpack (o,
                            db,
                            t,
                            move (pp),
                            o.replace (),
                            o.purge ());

          external = false;
        }
      }
      catch (const invalid_path& e)
      {
        fail << "invalid package path '" << e.path << "'";
      }
    }
    else
    {
      if (o.switch_ ())
        fail << "existing archive must be specified with --switch" <<
          info << "run 'bpkg help pkg-unpack' for more information";

      // The package name[/version] case.
      //
      if (!args.more ())
        fail << "package name argument expected" <<
          info << "run 'bpkg help pkg-unpack' for more information";

      const char*  arg (args.next ());
      package_name n   (parse_package_name (arg));
      version      v   (parse_package_version (arg));

      external = !v.empty ();

      if (o.replace () && !external)
        fail << "--replace|-r can only be specified with --existing|-e or "
             << "if package version is also specified";

      // If the package version is not specified then we expect the package to
      // already be fetched and so unpack it from the archive. Otherwise, we
      // "unpack" it from the directory-based repository.
      //
      if (v.empty ())
      {
        // Note that opening of the fetch cache can be redundant, if the
        // package archive is used in place. Let's, however, keep things
        // simple for now.
        //
        fetch_cache cache (o, &db);

        if (cache.cache_src ())
          cache.open (trace);

        p = pkg_unpack (o,
                        cache,
                        db,
                        t,
                        n,
                        false /* simulate */);

        if (cache.cache_src ())
          cache.close ();
      }
      else
      {
        p = pkg_unpack (o,
                        db /* pdb */,
                        db /* rdb */,
                        t,
                        move (n),
                        move (v),
                        o.replace (),
                        false /* simulate */);
      }
    }

    if (verb && !o.no_result ())
    {
      if (!external)
        text << (!o.switch_ () ? "unpacked " : "switched ") << *p;
      else
        text << "using " << *p << " (external)";
    }

    return 0;
  }

  pkg_unpack_options
  merge_options (const default_options<pkg_unpack_options>& defs,
                 const pkg_unpack_options& cmd)
  {
    // NOTE: remember to update the documentation if changing anything here.

    return merge_default_options (
      defs,
      cmd,
      [] (const default_options_entry<pkg_unpack_options>& e,
          const pkg_unpack_options&)
      {
        const pkg_unpack_options& o (e.options);

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
