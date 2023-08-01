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
#include <bpkg/manifest-utility.hxx>

#include <bpkg/pkg-purge.hxx>
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
        {}});       // No prerequisites captured yet.

      db.persist (p);
    }

    assert (p->external ());

    t.commit ();
    return move (p);
  }

  template <typename T>
  static shared_ptr<selected_package>
  pkg_unpack (const common_options& o,
              database& db,
              transaction& t,
              package_name n,
              version v,
              const vector<T>& deps,
              const package_info* pi,
              dir_path d,
              repository_location rl,
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
                       purge,
                       simulate);
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
                  false /* load_buildfiles */,
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

    // Use the special root repository fragment as the repository fragment of
    // this package.
    //
    return pkg_unpack (o,
                       db,
                       t,
                       move (m.name),
                       move (m.version),
                       m.dependencies,
                       &pvi.info,
                       d,
                       repository_location (),
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
      rdb.find<available_package> (available_package_id (n, v)));

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

    if (verb > 1)
      text << "unpacking " << pl->location.leaf () << " "
           << "from " << pl->repository_fragment->name;

    const repository_location& rl (pl->repository_fragment->location);

    return pkg_unpack (o,
                       pdb,
                       t,
                       move (n),
                       move (v),
                       ap->dependencies,
                       nullptr   /* package_info */,
                       path_cast<dir_path> (rl.path () / pl->location),
                       rl,
                       false     /* purge */,
                       simulate);
  }

  shared_ptr<selected_package>
  pkg_unpack (const common_options& co,
              database& db,
              transaction& t,
              const package_name& name,
              bool simulate)
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

    // Extract the package directory.
    //
    // Also, since we must have verified the archive during fetch,
    // here we can just assume what the resulting directory will be.
    //
    const package_name& n (p->name);
    const version& v (p->version);

    dir_path d (c / dir_path (n.string () + '-' + v.string ()));

    auto_rmdir arm;

    if (!simulate)
    {
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

      try
      {
        pair<process, process> pr (start_extract (co, a, c));

        // While it is reasonable to assuming the child process issued
        // diagnostics, tar, specifically, doesn't mention the archive name.
        //
        if (!pr.second.wait () || !pr.first.wait ())
          fail << "unable to extract " << a << " to " << c;
      }
      catch (const process_error& e)
      {
        fail << "unable to extract " << a << " to " << c << ": " << e;
      }
    }

    p->src_root = d.leaf (); // For now assuming to be in configuration.
    p->purge_src = true;

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

    database db (c, trace, true /* pre_attach */);
    transaction t (db);

    shared_ptr<selected_package> p;
    bool external (o.existing ());

    if (o.existing ())
    {
      // The package directory case.
      //
      if (!args.more ())
        fail << "package directory argument expected" <<
          info << "run 'bpkg help pkg-unpack' for more information";

      p = pkg_unpack (o,
                      db,
                      t,
                      dir_path (args.next ()),
                      o.replace (),
                      o.purge (),
                      false /* simulate */);
    }
    else
    {
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
        fail << "--replace|-r can only be specified with external package";

      // If the package version is not specified then we expect the package to
      // already be fetched and so unpack it from the archive. Otherwise, we
      // "unpack" it from the directory-based repository.
      //
      p = v.empty ()
        ? pkg_unpack (o,
                      db /* pdb */,
                      t,
                      n,
                      false /* simulate */)
        : pkg_unpack (o,
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
      if (!external)
        text << "unpacked " << *p;
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
