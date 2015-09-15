// file      : bpkg/pkg-fetch.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-fetch>

#include <memory> // shared_ptr

#include <bpkg/manifest>

#include <bpkg/types>
#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/utility>
#include <bpkg/database>
#include <bpkg/pkg-verify>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  pkg_fetch (const pkg_fetch_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_fetch");

    dir_path d (o.directory ());
    level4 ([&]{trace << "configuration: " << d;});

    database db (open (d));

    path a;
    bool purge;

    if (o.existing ())
    {
      if (!args.more ())
        fail << "archive path argument expected" <<
          info << "run 'bpkg help pkg-fetch' for more information";

      a = path (args.next ());

      if (!exists (a))
        fail << "archive file '" << a << "' does not exist";

      purge = false;
    }
    else
    {
      if (!args.more ())
        fail << "package name argument expected" <<
          info << "run 'bpkg help pkg-fetch' for more information";

      string name (args.next ());

      if (!args.more ())
        fail << "package version argument expected" <<
          info << "run 'bpkg help pkg-fetch' for more information";

      string ver (args.next ());

      // TODO:
      //
      // - Will need to use some kind or auto_rm/exception guard on
      //   fetched file.
      //

      fail << "pkg-fetch " << name << " " << ver << " not yet implemented";
      purge = true;
    }

    level4 ([&]{trace << "package archive: " << a << ", purge: " << purge;});

    // Verify archive is a package and get its manifest.
    //
    package_manifest m (pkg_verify (a));
    level4 ([&]{trace << m.name << " " << m.version;});

    const auto& n (m.name);

    // Database time.
    //
    transaction t (db.begin ());

    // See if this package already exists in this configuration.
    //
    if (shared_ptr<package> p = db.find<package> (n))
      fail << "package " << n << " already exists in configuration " << d <<
        info << "existing version: " << p->version << ", state: " << p->state;

    // Make the archive and configuration paths absolute and normalized.
    // If the archive is inside the configuration, use the relative path.
    // This way we can move configuration around.
    //
    d.complete ().normalize ();
    a.complete ().normalize ();

    if (a.sub (d))
      a = a.leaf (d);

    // Add the package to the configuration.
    //
    package p {move (m.name),
               move (m.version),
               state::fetched,
               move (a),
               purge};

    db.persist (p);
    t.commit ();
  }
}
