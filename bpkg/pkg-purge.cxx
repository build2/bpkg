// file      : bpkg/pkg-purge.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-purge.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  pkg_purge_fs (database& db,
                transaction& t,
                const shared_ptr<selected_package>& p,
                bool simulate,
                bool archive)
  {
    tracer trace ("pkg_purge_archive");

    assert (p->state == package_state::fetched ||
            p->state == package_state::unpacked);

    tracer_guard tg (db, trace);

    const dir_path& c (db.config_orig);

    try
    {
      if (p->purge_src)
      {
        if (!simulate)
        {
          dir_path d (p->effective_src_root (c));

          if (exists (d)) // Don't complain if someone did our job for us.
            rm_r (d);
        }

        p->purge_src = false;
      }

      // Let's forget about the possibly non-purged source directory, as the
      // selected package may now be reused for unrelated package version.
      //
      p->src_root = nullopt;
      p->manifest_checksum = nullopt;
      p->buildfiles_checksum = nullopt;

      if (archive)
      {
        if (p->purge_archive)
        {
          if (!simulate)
          {
            path a (p->effective_archive (c));

            if (exists (a))
              rm (a);
          }

          p->purge_archive = false;
        }

        // Let's forget about the possibly non-purged archive (see above).
        //
        p->archive = nullopt;
      }
    }
    catch (const failed&)
    {
      p->state = package_state::broken;
      db.update (p);
      t.commit ();

      info << "package " << p->name << db << " is now broken; "
           << "use 'pkg-purge --force' to remove";
      throw;
    }
  }

  void
  pkg_purge (database& db,
             transaction& t,
             const shared_ptr<selected_package>& p,
             bool simulate)
  {
    assert (p->state == package_state::fetched ||
            p->state == package_state::unpacked);

    tracer trace ("pkg_purge");

    tracer_guard tg (db, trace);

    assert (!p->out_root);
    pkg_purge_fs (db, t, p, simulate, true);

    db.erase (p);
    t.commit ();

    p->state = package_state::transient;
  }

  int
  pkg_purge (const pkg_purge_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_purge");

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-purge' for more information";

    package_name n (parse_package_name (args.next (),
                                        false /* allow_version */));

    database db (c, trace, true /* pre_attach */);
    transaction t (db);

    shared_ptr<selected_package> p (db.find<selected_package> (n));

    if (p == nullptr)
      fail << "package " << n << " does not exist in configuration " << c;

    // Make sure the package is in a state from which it can be purged.
    //
    switch (p->state)
    {
    case package_state::fetched:
      {
        // If we have --keep, then this is a no-op. We could have
        // detected this and returned but we still want the normal
        // diagnostics. So instead the logic below takes care of
        // this situation.
        //
        break;
      }
    case package_state::unpacked:
      {
        if (o.keep () && !p->archive)
          fail << "package " << n << " has no archive to keep";

        break;
      }
    case package_state::broken:
      {
        if (!o.force ())
          fail << "broken package " << n << " can only be purged with --force";

        if (o.keep ())
          fail << "cannot keep broken package " << n;

        break;
      }
    default:
      {
        fail << p->state << " package " << n << " cannot be purged";
      }
    }

    // For a broken package we just verify that all the filesystem objects
    // were cleaned up by the user.
    //
    if (p->state == package_state::broken)
    {
      if (p->out_root)
      {
        dir_path d (p->effective_out_root (c));

        if (exists (d))
          fail << "output directory of broken package " << n
               << " still exists" <<
            info << "remove " << d << " manually then re-run pkg-purge";
      }

      if (p->purge_src)
      {
        dir_path d (p->effective_src_root (c));

        if (exists (d))
          fail << "source directory of broken package " << n
               << " still exists" <<
            info << "remove " << d << " manually then re-run pkg-purge";
      }

      if (p->purge_archive)
      {
        path a (p->effective_archive (c));

        if (exists (a))
          fail << "archive file of broken package " << n << " still exists" <<
            info << "remove " << a << " manually then re-run pkg-purge";
      }
    }
    else
    {
      assert (!p->out_root);
      pkg_purge_fs (db, t, p, false /* simulate */, !o.keep ());
    }

    // Finally, update the database state.
    //
    if (o.keep ())
    {
      if (p->state != package_state::fetched) // No-op we were talking about.
      {
        p->state = package_state::fetched;
        db.update (p);
        t.commit ();
      }
    }
    else
    {
      db.erase (p);
      t.commit ();
      p->state = package_state::transient;
    }

    if (verb && !o.no_result ())
      text << (o.keep () ? "keeping archive " : "purged ") << *p;

    return 0;
  }
}
