// file      : bpkg/rep-create.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-create>

#include <map>
#include <utility>  // pair, move()
#include <cassert>
#include <fstream>
#include <iostream>
#include <system_error>

#include <butl/filesystem> // dir_iterator

#include <bpkg/manifest>
#include <bpkg/manifest-parser>
#include <bpkg/manifest-serializer>

#include <bpkg/types>
#include <bpkg/utility>
#include <bpkg/pkg-verify>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  using package_key = pair<string, version>;
  using package_data = pair<path, package_manifest>;
  using package_map = map<package_key, package_data>;

  static void
  collect (package_map& map, const dir_path& d, const dir_path& root)
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
          collect (map, path_cast<dir_path> (d / p), root);
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
      package_manifest m (pkg_verify (a));

      level4 ([&]{trace << m.name << " " << m.version << " in " << a;});

      // Add package archive location relative to the repository root.
      //
      m.location = a.leaf (root);

      package_key k (m.name, m.version); // Argument evaluation order.
      auto r (map.emplace (move (k), package_data (a, move (m))));

      // Diagnose duplicates.
      //
      if (!r.second)
      {
        const package_manifest& m (r.first->second.second);

        fail << "duplicate package " << m.name << " " << m.version <<
          info << "first package archive is " << r.first->second.first <<
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
  rep_create (const rep_create_options&, cli::scanner& args)
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
    path rf (d / path ("repositories"));

    if (!exists (rf))
      fail << "file " << rf << " does not exist";

    try
    {
      ifstream ifs;
      ifs.exceptions (ofstream::badbit | ofstream::failbit);
      ifs.open (rf.string ());

      manifest_parser mp (ifs, rf.string ());
      repository_manifests ms (mp);
      level4 ([&]{trace << ms.size () - 1 << " prerequisite repository(s)";});
    }
    catch (const manifest_parsing& e)
    {
      fail (e.name, e.line, e.column) << e.description;
    }
    catch (const ifstream::failure&)
    {
      fail << "unable to read from " << rf;
    }

    // While we could have serialized as we go along, the order of
    // packages will be pretty much random and not reproducible. By
    // collecting all the manifests in a map we get a sorted list.
    //
    package_map pm;
    collect (pm, d, d);

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
        const package_manifest& m (p.second.second);

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
