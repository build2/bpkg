// file      : bpkg/rep-create.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-create>

#include <map>
#include <fstream>
#include <iostream>
#include <system_error>

#include <butl/filesystem> // dir_iterator

#include <bpkg/manifest>
#include <bpkg/manifest-serializer>

#include <bpkg/fetch>
#include <bpkg/types>
#include <bpkg/utility>
#include <bpkg/diagnostics>

#include <bpkg/pkg-verify>

using namespace std;
using namespace butl;

namespace bpkg
{
  struct package_key
  {
    string name;
    bpkg::version version;

    // There shouldn't be multiple revisions of the same package
    // in a repository, thus we compare versions ignoring the
    // revision.
    //
    bool
    operator< (const package_key& y) const
    {
      int r (name.compare (y.name));
      return r < 0 || (r == 0 && version.compare (y.version, true) < 0);
    }
  };

  struct package_data
  {
    path archive;
    package_manifest manifest;
  };

  using package_map = map<package_key, package_data>;

  static void
  collect (const common_options& co,
           package_map& map,
           const dir_path& d,
           const dir_path& root)
  try
  {
    tracer trace ("collect");

    for (const dir_entry& de: dir_iterator (d)) // system_error
    {
      path p (de.path ());

      // Ignore entries that start with a dot (think .git/).
      //
      if (p.string ().front () == '.')
      {
        level4 ([&]{trace << "skipping '" << p << "' in " << d;});
        continue;
      }

      switch (de.type ()) // Follow symlinks, system_error.
      {
      case entry_type::directory:
        {
          collect (co, map, path_cast<dir_path> (d / p), root);
          continue;
        }
      case entry_type::regular:
        break;
      default:
        fail << "unexpected entry '" << p << "' in directory " << d;
      }

      // Ignore well-known top-level files.
      //
      if (d == root)
      {
        if (p == path ("repositories") ||
            p == path ("packages"))
          continue;
      }

      // Verify archive is a package and get its manifest.
      //
      path a (d / p);
      package_manifest m (pkg_verify (co, a));

      level4 ([&]{trace << m.name << " " << m.version << " in " << a;});

      // Add package archive location relative to the repository root.
      //
      m.location = a.leaf (root);

      package_key k {m.name, m.version}; // Argument evaluation order.
      auto r (map.emplace (move (k), package_data {a, move (m)}));

      // Diagnose duplicates.
      //
      if (!r.second)
      {
        const package_manifest& m (r.first->second.manifest);

        // Strip the revision from the version we print in case the
        // packages only differ in revisions and thus shouldn't be
        // both in this repository.
        //
        fail << "duplicate package " << m.name << " "
             << m.version.string (true) <<
          info << "first package archive is " << r.first->second.archive <<
          info << "second package archive is " << a;
      }
    }
  }
  catch (const system_error& e)
  {
    error << "unable to scan directory " << d << ": " << e.what ();
    throw failed ();
  }

  void
  rep_create (const rep_create_options& o, cli::scanner& args)
  try
  {
    tracer trace ("rep_create");

    dir_path d (args.more () ? args.next () : ".");
    if (d.empty ())
      throw invalid_path (d.string ());

    level4 ([&]{trace << "creating repository in " << d;});

    // Load the 'repositories' file to make sure it is there and
    // is valid.
    //
    repository_manifests rms (fetch_repositories (d));
    level4 ([&]{trace << rms.size () - 1 << " prerequisite repository(s)";});

    // While we could have serialized as we go along, the order of
    // packages will be pretty much random and not reproducible. By
    // collecting all the manifests in a map we get a sorted list.
    //
    package_map pm;
    collect (o, pm, d, d);

    // Serialize.
    //
    path p (d / path ("packages"));

    try
    {
      ofstream ofs;
      ofs.exceptions (ofstream::badbit | ofstream::failbit);
      ofs.open (p.string ());

      manifest_serializer s (ofs, p.string ());

      for (const auto& p: pm)
      {
        const package_manifest& m (p.second.manifest);

        if (verb)
          text << "adding " << m.name << " " << m.version;

        m.serialize (s);
      }

      s.next ("", ""); // The end.
    }
    catch (const manifest_serialization& e)
    {
      fail << "unable to save manifest: " << e.description;
    }
    catch (const ofstream::failure&)
    {
      fail << "unable to write to " << p;
    }

    if (verb)
    {
      d.complete ();
      d.normalize ();
      text << pm.size () << " package(s) in " << d;
    }
  }
  catch (const invalid_path& e)
  {
    fail << "invalid path: '" << e.path () << "'";
  }
}
