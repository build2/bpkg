// file      : bpkg/pkg-fetch.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-fetch.hxx>

#include <libbpkg/manifest.hxx>

#include <bpkg/fetch.hxx>
#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/checksum.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/pkg-purge.hxx>
#include <bpkg/pkg-verify.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  // Can return a new selected package object, replacing the existing one.
  //
  static shared_ptr<selected_package>
  pkg_fetch (dir_path c,
             transaction& t,
             package_name n,
             version v,
             path a,
             repository_location rl,
             bool purge,
             bool simulate)
  {
    tracer trace ("pkg_fetch");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    // Make the archive and configuration paths absolute and normalized.
    // If the archive is inside the configuration, use the relative path.
    // This way we can move the configuration around.
    //
    normalize (c, "configuration");
    normalize (a, "archive");

    if (a.sub (c))
      a = a.leaf (c);

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
        nullopt, // No output directory yet.
        {}});    // No prerequisites captured yet.

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
  pkg_fetch_check (const dir_path& c,
                   transaction& t,
                   const package_name& n,
                   bool replace)
  {
    tracer trace ("pkg_fetch_check");

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
          dr << info << "use 'pkg-fetch --replace|-r' to replace";
      }
    }
  }

  shared_ptr<selected_package>
  pkg_fetch (const common_options& co,
             const dir_path& c,
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
                                    false /* expand_values */));

    l4 ([&]{trace << m.name << " " << m.version;});

    // Check/diagnose an already existing package.
    //
    pkg_fetch_check (c, t, m.name, replace);

    // Use the special root repository fragment as the repository fragment of
    // this package.
    //
    return pkg_fetch (c,
                      t,
                      move (m.name),
                      move (m.version),
                      move (a),
                      repository_location (),
                      purge,
                      simulate);
  }

  shared_ptr<selected_package>
  pkg_fetch (const common_options& co,
             const dir_path& c,
             transaction& t,
             package_name n,
             version v,
             bool replace,
             bool simulate)
  {
    tracer trace ("pkg_fetch");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    // Check/diagnose an already existing package.
    //
    pkg_fetch_check (c, t, n, replace);

    check_any_available (c, t);

    // Note that here we compare including the revision (unlike, say in
    // pkg-status). Which means one cannot just specify 1.0.0 and get 1.0.0+1
    // -- they must spell it out explicitly. This is probably ok since this is
    // a low-level command where some extra precision doesn't hurt.
    //
    shared_ptr<available_package> ap (
      db.find<available_package> (available_package_id (n, v)));

    if (ap == nullptr)
      fail << "package " << n << " " << v << " is not available";

    // Pick an archive-based repository fragment. Preferring a local one over
    // the remotes seems like a sensible thing to do.
    //
    const package_location* pl (nullptr);

    for (const package_location& l: ap->locations)
    {
      const repository_location& rl (l.repository_fragment.load ()->location);

      if (rl.archive_based () && (pl == nullptr || rl.local ()))
      {
        pl = &l;

        if (rl.local ())
          break;
      }
    }

    if (pl == nullptr)
      fail << "package " << n << " " << v
           << " is not available from an archive-based repository";

    if (verb > 1)
      text << "fetching " << pl->location.leaf () << " "
           << "from " << pl->repository_fragment->name;

    auto_rmfile arm;
    path a (c / pl->location.leaf ());

    if (!simulate)
    {
      pkg_fetch_archive (
        co, pl->repository_fragment->location, pl->location, a);

      arm = auto_rmfile (a);

      // We can't be fetching an archive for a transient object.
      //
      assert (ap->sha256sum);

      const string& sha256sum (sha256 (co, a));
      if (sha256sum != *ap->sha256sum)
      {
        fail << "checksum mismatch for " << n << " " << v <<
          info << pl->repository_fragment->name << " has " << *ap->sha256sum <<
          info << "fetched archive has " << sha256sum <<
          info << "consider re-fetching package list and trying again" <<
          info << "if problem persists, consider reporting this to "
             << "the repository maintainer";
      }
    }

    shared_ptr<selected_package> p (
      pkg_fetch (c,
                 t,
                 move (n),
                 move (v),
                 move (a),
                 pl->repository_fragment->location,
                 true /* purge */,
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

    database db (open (c, trace));
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
                     c,
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
                     c,
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
