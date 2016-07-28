// file      : bpkg/rep-create.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-create>

#include <map>
#include <iostream>

#include <butl/fdstream>
#include <butl/filesystem> // dir_iterator

#include <bpkg/manifest>
#include <bpkg/manifest-serializer>

#include <bpkg/auth>
#include <bpkg/fetch>
#include <bpkg/archive>
#include <bpkg/checksum>
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

  static const path repositories ("repositories");
  static const path packages ("packages");
  static const path signature ("signature");

  static void
  collect (const rep_create_options& o,
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
        l4 ([&]{trace << "skipping '" << p << "' in " << d;});
        continue;
      }

      switch (de.type ()) // Follow symlinks, system_error.
      {
      case entry_type::directory:
        {
          collect (o, map, path_cast<dir_path> (d / p), root);
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
        if (p == repositories || p == packages || p == signature)
          continue;
      }

      // Verify archive is a package and get its manifest.
      //
      path a (d / p);
      package_manifest m (pkg_verify (o, a, o.ignore_unknown ()));

      // Calculate its checksum.
      //
      m.sha256sum = sha256 (o, a);

      l4 ([&]{trace << m.name << " " << m.version << " in " << a
                    << " sha256sum " << *m.sha256sum;});

      // Add package archive location relative to the repository root.
      //
      m.location = a.leaf (root);

      dir_path pd (m.name + "-" + m.version.string ());

      // Expand the description-file manifest value.
      //
      if (m.description && m.description->file)
      {
        path f (pd / m.description->path);
        string s (extract (o, a, f));

        if (s.empty ())
          fail << "description-file value in manifest of package archive "
               << a << " references empty file " << f;

        m.description = text_file (move (s));
      }

      // Expand the changes-file manifest values.
      //
      for (auto& c: m.changes)
      {
        if (c.file)
        {
          path f (pd / c.path);
          string s (extract (o, a, f));

          if (s.empty ())
            fail << "changes-file value in manifest of package archive " << a
                 << " references empty file " << f;

          c = text_file (move (s));
        }
      }

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

  int
  rep_create (const rep_create_options& o, cli::scanner& args)
  try
  {
    tracer trace ("rep_create");

    dir_path d (args.more () ? args.next () : ".");
    if (d.empty ())
      throw invalid_path (d.string ());

    l4 ([&]{trace << "creating repository in " << d;});

    // Load the 'repositories' file to make sure it is there and
    // is valid.
    //
    repository_manifests rms (fetch_repositories (d, o.ignore_unknown ()));
    l4 ([&]{trace << rms.size () - 1 << " prerequisite repository(s)";});

    // While we could have serialized as we go along, the order of
    // packages will be pretty much random and not reproducible. By
    // collecting all the manifests in a map we get a sorted list.
    //
    package_map pm;
    collect (o, pm, d, d);

    package_manifests manifests;
    manifests.sha256sum = sha256 (o, path (d / repositories));

    for (auto& p: pm)
    {
      package_manifest& m (p.second.manifest);

      if (verb)
        text << "adding " << m.name << " " << m.version;

      manifests.emplace_back (move (m));
    }

    // Serialize packages manifest, optionally generate the signature manifest.
    //
    path p (d / packages);

    try
    {
      {
        // While we can do nothing about repositories files edited on Windows
        // and littered with the carriage return characters, there is no
        // reason to litter the auto-generated packages and signature files.
        //
        ofdstream ofs (p, ios::binary);

        manifest_serializer s (ofs, p.string ());
        manifests.serialize (s);
        ofs.close ();
      }

      const optional<string>& cert (rms.back ().certificate);
      if (cert)
      {
        const string& key (o.key ());
        if (key.empty ())
          fail << "--key option required" <<
            info << "repository manifest contains a certificate" <<
            info << "run 'bpkg help rep-create' for more information";

        signature_manifest m;
        m.sha256sum = sha256 (o, p);
        m.signature = sign_repository (o, m.sha256sum, key, *cert, d);

        p = path (d / signature);

        ofdstream ofs (p, ios::binary);

        manifest_serializer s (ofs, p.string ());
        m.serialize (s);
        ofs.close ();
      }
      else
      {
        if (o.key_specified ())
          warn << "--key option ignored" <<
            info << "repository manifest contains no certificate" <<
            info << "run 'bpkg help rep-create' for more information";

        try_rmfile (path (d / signature), true);
      }
    }
    catch (const manifest_serialization& e)
    {
      fail << "unable to save manifest: " << e.description;
    }
    catch (const ofdstream::failure& e)
    {
      fail << "unable to write to " << p << ": " << e.what ();
    }

    if (verb)
    {
      d.complete ();
      d.normalize ();
      text << pm.size () << " package(s) in " << d;
    }

    return 0;
  }
  catch (const invalid_path& e)
  {
    error << "invalid path: '" << e.path << "'";
    throw failed ();
  }
}
