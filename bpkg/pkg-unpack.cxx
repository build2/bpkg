// file      : bpkg/pkg-unpack.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-unpack.hxx>

#include <libbpkg/manifest.hxx>

#include <bpkg/archive.hxx>
#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/checksum.hxx>
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
  pkg_unpack_check (const dir_path& c,
                    transaction& t,
                    const package_name& n,
                    bool replace)
  {
    tracer trace ("pkg_update_check");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    if (shared_ptr<selected_package> p = db.find<selected_package> (n))
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
          dr << info << "use 'pkg-unpack --replace|-r' to replace";
      }
    }
  }

  // Select the external package in this configuration. Can return a new
  // selected package object, replacing the existing one.
  //
  static shared_ptr<selected_package>
  pkg_unpack (const common_options& o,
              dir_path c,
              transaction& t,
              package_name n,
              version v,
              dir_path d,
              repository_location rl,
              bool purge,
              bool simulate)
  {
    tracer trace ("pkg_unpack");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    optional<string> mc;

    if (!simulate)
      mc = sha256 (o, d / manifest_file);

    // Make the package and configuration paths absolute and normalized.
    // If the package is inside the configuration, use the relative path.
    // This way we can move the configuration around.
    //
    normalize (c, "configuration");
    normalize (d, "package");

    if (d.sub (c))
      d = d.leaf (c);

    shared_ptr<selected_package> p (db.find<selected_package> (n));

    if (p != nullptr)
    {
      // Clean up the source directory and archive of the package we are
      // replacing. Once this is done, there is no going back. If things
      // go badly, we can't simply abort the transaction.
      //
      pkg_purge_fs (c, t, p, simulate);

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
        nullopt,    // No output directory yet.
        {}});       // No prerequisites captured yet.

      db.persist (p);
    }

    assert (p->external ());

    t.commit ();
    return p;
  }

  shared_ptr<selected_package>
  pkg_unpack (const common_options& o,
              const dir_path& c,
              transaction& t,
              const dir_path& d,
              bool replace,
              bool purge,
              bool simulate)
  {
    tracer trace ("pkg_unpack");

    if (!exists (d))
      fail << "package directory " << d << " does not exist";

    // Verify the directory is a package and get its manifest.
    //
    package_manifest m (
      pkg_verify (d,
                  true /* ignore_unknown */,
                  [&o, &d] (version& v)
                  {
                    if (optional<version> pv = package_version (o, d))
                      v = move (*pv);
                  }));

    l4 ([&]{trace << d << ": " << m.name << " " << m.version;});

    // Check/diagnose an already existing package.
    //
    pkg_unpack_check (c, t, m.name, replace);

    // Fix-up the package version.
    //
    if (optional<version> v = package_iteration (
          o, c, t, d, m.name, m.version, true /* check_external */))
      m.version = move (*v);

    // Use the special root repository fragment as the repository fragment of
    // this package.
    //
    return pkg_unpack (o,
                       c,
                       t,
                       move (m.name),
                       move (m.version),
                       d,
                       repository_location (),
                       purge,
                       simulate);
  }

  shared_ptr<selected_package>
  pkg_unpack (const common_options& o,
              const dir_path& c,
              transaction& t,
              package_name n,
              version v,
              bool replace,
              bool simulate)
  {
    tracer trace ("pkg_unpack");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    // Check/diagnose an already existing package.
    //
    pkg_unpack_check (c, t, n, replace);

    check_any_available (c, t);

    // Note that here we compare including the revision (see pkg-fetch()
    // implementation for more details).
    //
    shared_ptr<available_package> ap (
      db.find<available_package> (available_package_id (n, v)));

    if (ap == nullptr)
      fail << "package " << n << " " << v << " is not available";

    // Pick a directory-based repository fragment. They are always local, so we
    // pick the first one.
    //
    const package_location* pl (nullptr);

    for (const package_location& l: ap->locations)
    {
      if (l.repository_fragment.load ()->location.directory_based ())
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
                       c,
                       t,
                       move (n),
                       move (v),
                       path_cast<dir_path> (rl.path () / pl->location),
                       rl,
                       false /* purge */,
                       simulate);
  }

  shared_ptr<selected_package>
  pkg_unpack (const common_options& co,
              const dir_path& c,
              transaction& t,
              const package_name& name,
              bool simulate)
  {
    tracer trace ("pkg_unpack");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    shared_ptr<selected_package> p (db.find<selected_package> (name));

    if (p == nullptr)
      fail << "package " << name << " does not exist in configuration " << c;

    if (p->state != package_state::fetched)
      fail << "package " << name << " is " << p->state <<
        info << "expected it to be fetched";

    l4 ([&]{trace << *p;});

    assert (p->archive); // Should have archive in the fetched state.

    // Extract the package directory.
    //
    // Also, since we must have verified the archive during fetch,
    // here we can just assume what the resulting directory will be.
    //
    dir_path d (c / dir_path (p->name.string () + '-' + p->version.string ()));

    if (exists (d))
      fail << "package directory " << d << " already exists";

    auto_rmdir arm;
    optional<string> mc;

    if (!simulate)
    {
      // If the archive path is not absolute, then it must be relative
      // to the configuration.
      //
      path a (p->archive->absolute () ? *p->archive : c / *p->archive);

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

      mc = sha256 (co, d / manifest_file);
    }

    p->src_root = d.leaf (); // For now assuming to be in configuration.
    p->purge_src = true;

    p->manifest_checksum = move (mc);

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

    database db (open (c, trace));
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
                      c,
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
        ? pkg_unpack (o, c, t, n, false /* simulate */)
        : pkg_unpack (o,
                      c,
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
