// file      : bpkg/build.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/build>

#include <vector>

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
  struct selected_package // @@ Swap names.
  {
    shared_ptr<available_package> ap; // Note: might be transient.

    optional<path> archive;
    optional<dir_path> directory;
  };

  using packages = vector<selected_package>;

  void
  build (const build_options& o, cli::scanner& args)
  {
    tracer trace ("build");

    const dir_path& c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help build' for more information";

    database db (open (c, trace));
    transaction t (db.begin ());
    session s;

    shared_ptr<repository> root (db.load<repository> (""));

    // Start assembling the list of packages we will need to build
    // by first collecting the user's selection.
    //
    packages pkgs;

    while (args.more ())
    {
      const char* s (args.next ());

      selected_package pkg;

      // Reduce all the potential variations (archive, directory,
      // package, package version) to the single available_package
      // object.
      //
      string n;
      version v;
      shared_ptr<available_package>& ap (pkg.ap);

      // Is this a package archive?
      //
      try
      {
        path a (s);
        if (exists (a))
        {
          package_manifest m (pkg_verify (o, a, false));

          // This is a package archive (note that we shouldn't throw
          // failed from here on).
          //
          level4 ([&]{trace << "archive " << a;});
          n = m.name;
          v = m.version;
          ap = make_shared<available_package> (move (m));
          pkg.archive = move (a);
        }
      }
      catch (const invalid_path&)
      {
        // Not a valid path so cannot be an archive.
      }
      catch (const failed&)
      {
        // Not a valid package archive.
      }

      // Is this a package directory?
      //
      try
      {
        dir_path d (s);
        if (exists (d))
        {
          package_manifest m (pkg_verify (d, false));

          // This is a package directory (note that we shouldn't throw
          // failed from here on).
          //
          level4 ([&]{trace << "directory " << d;});
          n = m.name;
          v = m.version;
          ap = make_shared<available_package> (move (m));
          pkg.directory = move (d);
        }
      }
      catch (const invalid_path&)
      {
        // Not a valid path so cannot be an archive.
      }
      catch (const failed&)
      {
        // Not a valid package archive.
      }

      // Then it got to be a package name with optional version.
      //
      if (ap == nullptr)
      {
        n = parse_package_name (s);
        v = parse_package_version (s);
        level4 ([&]{trace << "package " << n << "; version " << v;});

        // Find the available package, if any.
        //
        using query = query<available_package>;

        query q (query::id.name == n);

        // Either get the user-specified version or the latest.
        //
        if (!v.empty ())
          q = q && query::id.version == v;
        else
          q += order_by_version_desc (query::id.version);

        // Only consider packages that are in repositories that were
        // explicitly added to the configuration and their complements,
        // recursively.
        //
        ap = filter_one (root, db.query<available_package> (q)).first;
      }

      // Load the package that may have already been selected and
      // figure out what exactly we need to do here. The end goal
      // is the available_package object corresponding to the actual
      // package that we will be building (which may or may not be
      // the same as the selected package).
      //
      shared_ptr<package> p (db.find<package> (n));

      if (p != nullptr && p->state == state::broken)
        fail << "unable to build broken package " << n <<
          info << "use 'pkg-purge --force' to remove";

      // If the user asked for a specific version, then that's what
      // we should be building.
      //
      if (!v.empty ())
      {
        for (;;)
        {
          if (ap != nullptr) // Must be that version, see above.
            break;

          // Otherwise, our only chance is that the already selected
          // object is that exact version.
          //
          if (p != nullptr && p->version == v)
            break; // Set ap to p below.

          fail << "unknown package " << n << " " << v;
        }
      }
      //
      // No explicit version was specified by the user.
      //
      else
      {
        if (ap != nullptr)
        {
          // See if what we already have is newer.
          //
          if (p != nullptr && ap->id.version < p->version)
            ap = nullptr; // Set ap to p below.
        }
        else
        {
          if (p == nullptr)
            fail << "unknown package " << n;

          // Set ap to p below.
        }
      }

      // If the available_package object is still NULL, then it means
      // we need to get one corresponding to the selected package.
      //
      if (ap == nullptr)
      {
        assert (p != nullptr);

        // The package is in at least fetched state, which means we
        // can get its manifest.
        //
        ap = make_shared<available_package> (
          p->state == state::fetched
          ? pkg_verify (o, *p->archive)
          : pkg_verify (*p->src_root));
      }

      level4 ([&]{trace << "building " << ap->id.name << " " << ap->version;});
      pkgs.push_back (move (pkg));
    }

    t.commit ();
  }
}
