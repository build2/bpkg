// file      : bpkg/pkg-fetch.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-fetch>

#include <bpkg/manifest>

#include <bpkg/fetch>
#include <bpkg/types>
#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/utility>
#include <bpkg/database>
#include <bpkg/diagnostics>
#include <bpkg/manifest-utility>

#include <bpkg/pkg-purge>
#include <bpkg/pkg-verify>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  pkg_fetch (const pkg_fetch_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_fetch");

    dir_path c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    database db (open (c, trace));
    transaction t (db.begin ());
    session s;

    path a;
    auto_rm arm;
    bool purge;
    repository_location rl;
    shared_ptr<selected_package> sp;

    // Check if the package already exists in this configuration and
    // diagnose all the illegal cases. We want to do this as soon as
    // the package name is known which happens at different times
    // depending on whether we are dealing with an existing archive
    // or fetching one.
    //
    auto check = [&o, &c, &db] (const string& n)
      -> shared_ptr<selected_package>
    {
      shared_ptr<selected_package> p (db.find<selected_package> (n));

      if (p == nullptr ||
          (p->state == package_state::fetched && o.replace ()))
        return p;

      {
        diag_record dr (error);

        dr << "package " << n << " already exists in configuration " << c <<
          info << "version: " << p->version << ", state: " << p->state;

        if (p->state == package_state::fetched)
          dr << info << "use 'pkg-fetch --replace|-r' to replace its archive";
      }

      throw failed ();
    };

    if (o.existing ())
    {
      if (!args.more ())
        fail << "archive path argument expected" <<
          info << "run 'bpkg help pkg-fetch' for more information";

      a = path (args.next ());

      if (!exists (a))
        fail << "archive file '" << a << "' does not exist";

      purge = o.purge ();

      // Use the special root repository as the repository of this
      // package.
      //
      rl = repository_location ();
    }
    else
    {
      if (!args.more ())
        fail << "package name/version argument expected" <<
          info << "run 'bpkg help pkg-fetch' for more information";

      const char* arg (args.next ());
      string n (parse_package_name (arg));
      version v (parse_package_version (arg));

      if (v.empty ())
        fail << "package version expected" <<
          info << "run 'bpkg help pkg-fetch' for more information";

      // Check/diagnose an already existing package.
      //
      sp = check (n);

      if (db.query_value<repository_count> () == 0)
        fail << "configuration " << c << " has no repositories" <<
          info << "use 'bpkg rep-add' to add a repository";

      if (db.query_value<available_package_count> () == 0)
        fail << "configuration " << c << " has no available packages" <<
          info << "use 'bpkg rep-fetch' to fetch available packages list";

      shared_ptr<available_package> ap (
        db.find<available_package> (available_package_id (n, v)));

      if (ap == nullptr)
        fail << "package " << n << " " << v << " is not available";

      // Pick a repository. Preferring a local one over the remotes seems
      // like a sensible thing to do.
      //
      const package_location* pl (&ap->locations.front ());

      for (const package_location& l: ap->locations)
      {
        if (!l.repository.load ()->location.remote ())
        {
          pl = &l;
          break;
        }
      }

      if (verb > 1)
        text << "fetching " << pl->location.leaf () << " "
             << "from " << pl->repository->name;

      rl = pl->repository->location;
      a = fetch_archive (o, rl, pl->location, c);
      arm = auto_rm (a);
      purge = true;
    }

    level4 ([&]{trace << "package archive: " << a << ", purge: " << purge;});

    // Verify archive is a package and get its manifest.
    //
    package_manifest m (pkg_verify (o, a));
    level4 ([&]{trace << m.name << " " << m.version;});

    // Check/diagnose an already existing package.
    //
    if (o.existing ())
      sp = check (m.name);

    // Make the archive and configuration paths absolute and normalized.
    // If the archive is inside the configuration, use the relative path.
    // This way we can move the configuration around.
    //
    c.complete ().normalize ();
    a.complete ().normalize ();

    if (a.sub (c))
      a = a.leaf (c);

    if (sp != nullptr)
    {
      // Clean up the archive we are replacing. Once this is done, there
      // is no going back. If things go badly, we can't simply abort the
      // transaction.
      //
      if (sp->purge_archive)
        pkg_purge_archive (c, t, sp);

      sp->version = move (m.version);
      sp->repository = move (rl);
      sp->archive = move (a);
      sp->purge_archive = purge;

      db.update (sp);
    }
    else
    {
      // Add the package to the configuration.
      //
      sp.reset (new selected_package {
        move (m.name),
        move (m.version),
        package_state::fetched,
        move (rl),
        move (a),
        purge,
        nullopt, // No source directory yet.
        false,
        nullopt, // No output directory yet.
        {}});    // No prerequisites captured yet.

      db.persist (sp);
    }

    t.commit ();
    arm.cancel ();

    if (verb)
      text << "fetched " << sp->name << " " << sp->version;
  }
}
