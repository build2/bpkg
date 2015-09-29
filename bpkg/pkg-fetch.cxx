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

    if (o.existing ())
    {
      if (!args.more ())
        fail << "archive path argument expected" <<
          info << "run 'bpkg help pkg-fetch' for more information";

      a = path (args.next ());

      if (!exists (a))
        fail << "archive file '" << a << "' does not exist";

      purge = o.purge ();
    }
    else
    {
      if (!args.more ())
        fail << "package name argument expected" <<
          info << "run 'bpkg help pkg-fetch' for more information";

      string n (args.next ());

      if (!args.more ())
        fail << "package version argument expected" <<
          info << "run 'bpkg help pkg-fetch' for more information";

      version v (parse_version (args.next ()));

      if (db.query_value<repository_count> () == 0)
        fail << "configuration " << c << " has no repositories" <<
          info << "use 'bpkg rep-add' to add a repository";

      if (db.query_value<available_package_count> () == 0)
        fail << "configuration " << c << " has no available packages" <<
          info << "use 'bpkg rep-fetch' to fetch available packages list";

      shared_ptr<available_package> p (
        db.find<available_package> (available_package_id (n, v)));

      if (p == nullptr)
        fail << "package " << n << " " << v << " is not available";

      // Pick a repository. Prefer local ones over the remote.
      //
      const package_location* pl (&p->locations.front ());

      for (const package_location& l: p->locations)
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

      a = fetch_archive (o, pl->repository->location, pl->location, c);
      arm = auto_rm (a);
      purge = true;
    }

    level4 ([&]{trace << "package archive: " << a << ", purge: " << purge;});

    // Verify archive is a package and get its manifest.
    //
    package_manifest m (pkg_verify (o, a));
    level4 ([&]{trace << m.name << " " << m.version;});

    const auto& n (m.name);

    // See if this package already exists in this configuration.
    //
    if (shared_ptr<package> p = db.find<package> (n))
      fail << "package " << n << " already exists in configuration " << c <<
        info << "version: " << p->version << ", state: " << p->state;

    // Make the archive and configuration paths absolute and normalized.
    // If the archive is inside the configuration, use the relative path.
    // This way we can move the configuration around.
    //
    c.complete ().normalize ();
    a.complete ().normalize ();

    if (a.sub (c))
      a = a.leaf (c);

    // Add the package to the configuration.
    //
    shared_ptr<package> p (new package {
        move (m.name),
        move (m.version),
        state::fetched,
        move (a),
        purge,
        nullopt, // No source directory yet.
        false,
        nullopt  // No output directory yet.
     });

    db.persist (p);

    t.commit ();
    arm.cancel ();

    if (verb)
      text << "fetched " << p->name << " " << p->version;
  }
}
