// file      : bpkg/pkg-purge.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-purge>

#include <bpkg/types>
#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/utility>
#include <bpkg/database>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  pkg_purge (const pkg_purge_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_purge");

    const dir_path& c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-purge' for more information";

    string n (args.next ());

    database db (open (c, trace));
    transaction t (db.begin ());

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

    // First clean up the package source directory.
    //
    if (p->purge_src)
    {
      dir_path d (p->src_root->absolute () ? *p->src_root : c / *p->src_root);

      if (p->state != package_state::broken)
      {
        try
        {
          if (exists (d)) // Don't complain if someone did our job for us.
            rm_r (d);

          p->src_root = nullopt;
          p->purge_src = false;
        }
        catch (const failed&)
        {
          p->state = package_state::broken;
          db.update (p);
          t.commit ();

          info << "package " << n << " is now broken; "
               << "use 'pkg-purge --force' to remove";
          throw;
        }
      }
      else
      {
        // If we are broken, simply make sure the user cleaned things up
        // manually.
        //
        if (exists (d))
          fail << "broken package " << n << " source directory still exists" <<
            info << "remove " << d << " manually then re-run pkg-purge";
      }
    }

    // Also check the output directory of broken packages.
    //
    if (p->out_root)
    {
      // Can only be present if broken.
      //
      assert (p->state == package_state::broken);

      dir_path d (c / *p->out_root); // Always relative.

      if (exists (d))
        fail << "broken package " << n << " output directory still exists" <<
          info << "remove " << d << " manually then re-run pkg-purge";
    }

    // Now the archive. Pretty much the same code as above but for a file.
    //
    if (p->purge_archive && !o.keep ())
    {
      path a (p->archive->absolute () ? *p->archive : c / *p->archive);

      if (p->state != package_state::broken)
      {
        try
        {
          if (exists (a))
            rm (a);

          p->archive = nullopt;
          p->purge_archive = false;
        }
        catch (const failed&)
        {
          p->state = package_state::broken;
          db.update (p);
          t.commit ();

          info << "package " << n << " is now broken; "
               << "use 'pkg-purge --force' to remove";
          throw;
        }
      }
      else
      {
        if (exists (a))
          fail << "broken package " << n << " archive still exists" <<
            info << "remove " << a << " manually then re-run pkg-purge";
      }
    }

    if (o.keep ())
    {
      if (p->state != package_state::fetched) // No-op we were talking about.
      {
        p->state = package_state::fetched;
        db.update (p);
      }
    }
    else
      db.erase (p);

    t.commit ();

    if (verb)
        text << (o.keep () ? "keeping archive " : "purged ")
             << p->name << " " << p->version;
  }
}
