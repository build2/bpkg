// file      : bpkg/rep-create.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-create.hxx>

#include <map>

#include <libbutl/filesystem.hxx>          // dir_iterator
#include <libbutl/manifest-serializer.hxx>

#include <libbpkg/manifest.hxx>
#include <libbpkg/package-name.hxx>

#include <bpkg/auth.hxx>
#include <bpkg/fetch.hxx>
#include <bpkg/checksum.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/pkg-verify.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  struct package_name_version
  {
    package_name name;
    bpkg::version version;

    // There shouldn't be multiple revisions of the same package
    // in a repository, thus we compare versions ignoring the
    // revision.
    //
    bool
    operator< (const package_name_version& y) const
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

  using package_map = map<package_name_version, package_data>;

  static void
  collect (const rep_create_options& o,
           package_map& map,
           const dir_path& d,
           const dir_path& root)
  try
  {
    tracer trace ("collect");

    for (const dir_entry& de: dir_iterator (d, dir_iterator::no_follow))
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
        if (p == repositories_file ||
            p == packages_file     ||
            p == signature_file)
          continue;
      }

      // Verify archive is a package and get its manifest.
      //
      path a (d / p);

      package_manifest m (
        pkg_verify (o,
                    a,
                    o.ignore_unknown (),
                    o.ignore_unknown () /* ignore_toolchain */,
                    true /* expand_values */,
                    true /* load_buildfiles */));

      // Calculate its checksum.
      //
      m.sha256sum = sha256sum (o, a);

      l4 ([&]{trace << m.name << " " << m.version << " in " << a
                    << " sha256sum " << *m.sha256sum;});

      // Add package archive location relative to the repository root.
      //
      m.location = a.leaf (root);

      package_name_version k {m.name, m.version}; // Argument evaluation order.
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
    fail << "unable to scan directory " << d << ": " << e << endf;
  }

  int
  rep_create (const rep_create_options& o, cli::scanner& args)
  try
  {
    tracer trace ("rep_create");

    dir_path d (args.more () ? args.next () : ".");
    if (d.empty ())
      throw invalid_path (d.representation ());

    l4 ([&]{trace << "creating repository in " << d;});

    // Load the repositories.manifest file to obtain the certificate, if
    // present, for signing the repository.
    //
    pkg_repository_manifests rms (
      pkg_fetch_repositories (d, o.ignore_unknown ()));

    l4 ([&]{trace << count_if (rms.begin(), rms.end(),
                               [] (const repository_manifest& i)
                               {
                                 return i.effective_role () !=
                                   repository_role::base;
                               })
                  << " prerequisite repository(s)";});

    optional<standard_version> rmv (
      rms.header && rms.header->min_bpkg_version
      ? rms.header->min_bpkg_version
      : nullopt);

    optional<standard_version> opv (o.min_bpkg_version_specified ()
                                    ? o.min_bpkg_version ()
                                    : optional<standard_version> ());

    // While we could have serialized as we go along, the order of
    // packages will be pretty much random and not reproducible. By
    // collecting all the manifests in a map we get a sorted list.
    //
    package_map pm;
    collect (o, pm, d, d);

    pkg_package_manifests manifests;
    manifests.sha256sum = sha256sum (o, path (d / repositories_file));

    for (auto& p: pm)
    {
      package_manifest& m (p.second.manifest);

      if (verb && !o.no_result ())
        text << "added " << m.name << " " << m.version;

      manifests.emplace_back (move (m));
    }

    // Issue a warning if --min-bpkg-version option and the repositories
    // manifest's min-bpkg-version value are both specified and don't match.
    // Let's issue it after the added repositories are printed to stdout, so
    // that it doesn't go unnoticed.
    //
    if (opv && rmv && *opv != *rmv)
      warn << "--min-bpkg-version option value " << *opv << " differs from "
           << "minimum bpkg version " << *rmv << " specified in "
           << d / repositories_file;

    // Serialize packages manifest, optionally generate the signature manifest.
    //
    path p (d / packages_file);

    try
    {
      {
        // While we can do nothing about repositories manifest files edited on
        // Windows and littered with the carriage return characters, there is
        // no reason to litter the auto-generated packages and signature
        // manifest files.
        //
        ofdstream ofs (p, fdopen_mode::binary);

        manifest_serializer s (ofs, p.string ());
        manifests.serialize (s, opv ? opv : rmv);
        ofs.close ();
      }

      const optional<string>& cert (find_base_repository (rms).certificate);

      if (cert)
      {
        const string& key (o.key ());
        if (key.empty ())
          fail << "--key option required" <<
            info << "repository manifest contains a certificate" <<
            info << "run 'bpkg help rep-create' for more information";

        signature_manifest m;
        m.sha256sum = sha256sum (o, p);
        m.signature = sign_repository (o, m.sha256sum, key, *cert, d);

        p = path (d / signature_file);

        ofdstream ofs (p, fdopen_mode::binary);

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

        try_rmfile (path (d / signature_file), true);
      }
    }
    catch (const manifest_serialization& e)
    {
      fail << "unable to save manifest: " << e.description;
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << p << ": " << e;
    }

    if (verb && !o.no_result ())
    {
      normalize (d, "repository");
      text << pm.size () << " package(s) in " << d;
    }

    return 0;
  }
  catch (const invalid_path& e)
  {
    fail << "invalid path: '" << e.path << "'" << endf;
  }

  default_options_files
  options_files (const char*, const rep_create_options&, const strings& args)
  {
    // NOTE: remember to update the documentation if changing anything here.

    // bpkg.options
    // bpkg-rep-create.options

    // Use the repository directory as a start directory.
    //
    optional<dir_path> start;

    // Let rep_create() complain later for invalid repository directory.
    //
    try
    {
      dir_path d (!args.empty () ? args[0] : ".");
      if (!d.empty ())
        start = move (normalize (d, "repository"));
    }
    catch (const invalid_path&) {}

    return default_options_files {
      {path ("bpkg.options"), path ("bpkg-rep-create.options")},
      move (start)};
  }

  rep_create_options
  merge_options (const default_options<rep_create_options>& defs,
                 const rep_create_options& cmd)
  {
    // NOTE: remember to update the documentation if changing anything here.

    return merge_default_options (
      defs,
      cmd,
      [] (const default_options_entry<rep_create_options>& e,
          const rep_create_options&)
      {
        // For security reason.
        //
        if (e.options.key_specified () && e.remote)
          fail (e.file) << "--key <name> in remote default options file";
      });
  }
}
