// file      : bpkg/pkg-purge.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-purge>

#include <memory> // shared_ptr

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

    dir_path c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-purge' for more information";

    string n (args.next ());

    database db (open (c));
    transaction t (db.begin ());

    shared_ptr<package> p (db.find<package> (n));

    if (p == nullptr)
      fail << "package " << n << " does not exist in configuration " << c;

    // Make sure the package is in a state from which it can be purged.
    //
    switch (p->state)
    {
    case state::fetched:
      {
        // If we have --keep, then this is a no-op. We could have
        // detected this and returned but we still want the normal
        // diagnostics. So instead the logic below takes care of
        // this situation.
        //
        break;
      }
    case state::unpacked:
      {
        if (o.keep () && !p->archive)
          fail << "package " << n << " has no archive to keep";

        break;
      }
    case state::broken:
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
    if (p->source_purge)
    {
      dir_path d (*p->source);
      if (d.relative ())
        d = c / d;

      if (p->state != state::broken)
      {
        try
        {
          if (exists (d)) // Don't complain if someone did our job for us.
            rm_r (d);

          p->source = optional<dir_path> ();
          p->source_purge = false;
        }
        catch (const failed&)
        {
          p->state = state::broken;
          db.update (p);
          t.commit ();

          if (verb)
            text << "broke " << p->name << " " << p->version;

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

    // Now the archive. Pretty much the same code as above but for a file.
    //
    if (p->archive_purge && !o.keep ())
    {
      path a (*p->archive);
      if (a.relative ())
        a = c / a;

      if (p->state != state::broken)
      {
        try
        {
          if (exists (a))
            rm (a);

          p->archive = optional<path> ();
          p->archive_purge = false;
        }
        catch (const failed&)
        {
          p->state = state::broken;
          db.update (p);
          t.commit ();

          if (verb)
            text << "broke " << p->name << " " << p->version;

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
      if (p->state != state::fetched) // That no-op we were talking about.
      {
        p->state = state::fetched;
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
