// file      : bpkg/rep-create.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-create>

#include <map>
#include <utility>  // pair, move()
#include <cassert>
#include <fstream>
#include <iostream>

#include <butl/process>
#include <butl/fdstream>
#include <butl/filesystem>

#include <bpkg/manifest>
#include <bpkg/manifest-parser>
#include <bpkg/manifest-serializer>

#include <bpkg/types>
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
  {
    tracer trace ("collect");

    for (const dir_entry& de: dir_iterator (d))
    {
      path p (de.path ());

      // Ignore entries that start with a dot (think .git/).
      //
      if (p.string ().front () == '.')
      {
        level3 ([&]{trace << "skipping '" << p << "' in " << d;});
        continue;
      }

      switch (de.type ()) // Follow symlinks.
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

      path fp (d / p);

      // Figure out the package directory. Strip the top-level extension
      // and, as a special case, if the second-level extension is .tar,
      // strip that as well (e.g., .tar.bz2).
      //
      p = p.base ();
      if (const char* e = p.extension ())
      {
        if (e == string ("tar"))
          p = p.base ();
      }

      level4 ([&]{trace << "found package " << p << " in " << fp;});

      // Extract the manifest.
      //
      path mf (p / path ("manifest"));

      const char* args[] {
        "tar",
        "-xOf",                // -O/--to-stdout -- extract to STDOUT.
        fp.string ().c_str (),
        mf.string ().c_str (),
        nullptr};

      if (verb >= 2)
        print_process (args);

      try
      {
        process pr (args, 0, -1); // Open pipe to stdout.

        try
        {
          ifdstream is (pr.in_ofd);
          is.exceptions (ifdstream::badbit | ifdstream::failbit);

          manifest_parser mp (is, mf.string ());
          package_manifest m (mp);

          // Verify package archive/directory is <name>-<version>.
          //
          {
            path ep (m.name + "-" + m.version.string ());

            if (p != ep)
              fail << "package archive/directory name mismatch in " << fp <<
                info << "extracted from archive '" << p << "'" <<
                info << "expected from manifest '" << ep << "'";
          }

          // Add package archive location relative to the repository root.
          //
          m.location = fp.leaf (root);

          package_key k (m.name, m.version); // Argument evaluation order.
          auto r (map.emplace (move (k), package_data (fp, move (m))));

          // Diagnose duplicates.
          //
          if (!r.second)
          {
            const package_manifest& m (r.first->second.second);

            fail << "duplicate package " << m.name << " " << m.version <<
              info << "first package archive is " << r.first->second.first <<
              info << "second package archive is " << fp;
          }
        }
        // Ignore these exceptions if the child process exited with
        // an error status since that's the source of the failure.
        //
        catch (const manifest_parsing& e)
        {
          if (pr.wait ())
            fail (e.name, e.line, e.column) << e.description <<
              info << "package archive " << fp;
        }
        catch (const ifdstream::failure&)
        {
          if (pr.wait ())
            fail << "unable to extract " << mf << " from " << fp;
        }

        if (!pr.wait ())
        {
          // While it is reasonable to assuming the child process issued
          // diagnostics, tar, specifically, doesn't mention the archive
          // name.
          //
          fail << fp << " does not appear to be a bpkg package";
        }
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        if (e.child ())
          exit (1);

        throw failed ();
      }
    }
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

    if (!file_exists (rf))
      fail << "file " << rf << " does not exist";

    try
    {
      ifstream ifs;
      ifs.exceptions (ofstream::badbit | ofstream::failbit);
      ifs.open (rf.string ());

      manifest_parser mp (ifs, rf.string ());
      repository_manifests ms (mp);
      level3 ([&]{trace << ms.size () - 1 << " prerequisite repository(s)";});
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
    catch (const ifdstream::failure&)
    {
      fail << "unable to write to " << p;
    }

    if (verb)
      text << pm.size () << " package(s)";
  }
  catch (const invalid_path& e)
  {
    fail << "invalid path: '" << e.path () << "'";
  }
}
