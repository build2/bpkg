// file      : bpkg/rep-info.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-info.hxx>

#include <iostream> // cout

#include <libbutl/manifest-serializer.hxx>

#include <libbpkg/manifest.hxx>

#include <bpkg/auth.hxx>
#include <bpkg/package.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/rep-fetch.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  int
  rep_info (const rep_info_options& o, cli::scanner& args)
  {
    tracer trace ("rep_info");

    if (!args.more ())
      fail << "repository location argument expected" <<
        info << "run 'bpkg help rep-info' for more information";

    if ((o.repositories_file_specified () || o.packages_file_specified ()) &&
        !o.manifest ())
      fail << (o.repositories_file_specified ()
               ? "--repositories-file"
               : "--packages-file")
           << " specified without --manifest" <<
        info << "run 'bpkg help rep-info' for more information";

    repository_location rl (
      parse_location (args.next (),
                      o.type_specified ()
                      ? optional<repository_type> (o.type ())
                      : nullopt));

    // Fetch everything we will need before printing anything. Ignore
    // unknown manifest entries unless we are dumping them.
    //
    dir_path d (o.directory ());

    const dir_path* conf (o.directory_specified () && d.empty ()
                          ? nullptr
                          : &d);

    // If --directory|-d is not specified and the current working directory is
    // a configuration directory, then initialize the temporary directory
    // inside it, so that we can always move a version control-based
    // repository into and out of it (see pkg_checkout() for details).
    //
    if (conf != nullptr && conf->empty ())
      conf = exists (bpkg_dir) ? &current_dir : nullptr;

    assert (conf == nullptr || !conf->empty ());

    init_tmp (conf != nullptr ? *conf : empty_dir_path);

    bool ignore_unknown (!o.manifest () || o.ignore_unknown ());

    rep_fetch_data rfd (
      rep_fetch (o,
                 conf,
                 rl,
                 ignore_unknown,
                 ignore_unknown /* ignore_toolchain */,
                 o.deep () /* expand_values */,
                 o.deep () /* load_buildfiles */));

    // Now print.
    //
    bool cert_info (o.cert_fingerprint ()  ||
                    o.cert_name ()         ||
                    o.cert_organization () ||
                    o.cert_email ());

    bool all (!o.name ()         &&
              !o.repositories () &&
              !o.packages ()     &&
              !cert_info);

    try
    {
      cout.exceptions (ostream::badbit | ostream::failbit);

      if (all || o.name ())
        cout << rl.canonical_name () << " " << rl << endl;

      // Certificate.
      //
      if (all || cert_info)
      {
        shared_ptr<const certificate>& cert (rfd.certificate);
        const optional<string>& cert_pem (rfd.certificate_pem);

        if (cert_pem)
        {
          // Repository is signed. If we got the repository certificate as the
          // result of authentication then use it for printing as well.
          // Otherwise parse it's PEM representation.
          //
          if (cert == nullptr)
            cert = parse_certificate (o, *cert_pem, rl);
          else
            assert (!cert->dummy ());
        }
        else if (cert != nullptr)
        {
          // Reset the dummy certificate pointer that we got as a result of
          // the unsigned repository authentication.
          //
          assert (cert->dummy ());
          cert = nullptr;
        }

        if (all)
        {
          // Print in the human-friendly format (nothing for an unsigned
          // repository).
          //
          if (cert != nullptr)
            cout << "CN=" << cert->name << "/O=" << cert->organization <<
              "/" << cert->email << endl
                 << cert->fingerprint << endl;
        }
        else
        {
          // Print in the structured format if any of --cert-* options are
          // specified. Print empty lines for an unsigned repository.
          //
          if (o.cert_fingerprint ())
          {
            if (cert != nullptr)
              cout << cert->fingerprint;
            cout << endl;
          }

          if (o.cert_name ())
          {
            if (cert != nullptr)
              cout << "name:" << cert->name;
            cout << endl;
          }

          if (o.cert_organization ())
          {
            if (cert != nullptr)
              cout << cert->organization;
            cout << endl;
          }

          if (o.cert_email ())
          {
            if (cert != nullptr)
              cout << cert->email;
            cout << endl;
          }
        }
      }

      // Repositories.
      //
      if (all || o.repositories ())
      {
        if (o.manifest ())
        {
          // Merge repository manifest lists, adding the fragment value to
          // prerequisite/complement repository manifests, and picking the
          // latest base repository manifest.
          //
          vector<repository_manifest> rms;

          for (rep_fetch_data::fragment& fr: rfd.fragments)
          {
            for (repository_manifest& rm: fr.repositories)
            {
              if (rm.effective_role () != repository_role::base)
              {
                if (!fr.id.empty ())
                  rm.fragment = fr.id;

                rms.push_back (move (rm));
              }
            }
          }

          // Append the latest base repository manifest or an empty base if
          // there are no fragments.
          //
          rms.push_back (
            !rfd.fragments.empty ()
            ? find_base_repository (rfd.fragments.back ().repositories)
            : repository_manifest ());

          auto serialize = [&rms] (ostream& os, const string& name)
          {
            // Note: serializing without any extra repository_manifests info.
            //
            manifest_serializer s (os, name);

            for (const repository_manifest& rm: rms)
              rm.serialize (s);

            s.next ("", ""); // End of stream.
          };

          if (o.repositories_file_specified ())
          {
            const path& p (o.repositories_file ());

            try
            {
              // Let's set the binary mode not to litter the manifest file
              // with the carriage return characters on Windows.
              //
              ofdstream ofs (p, fdopen_mode::binary);
              serialize (ofs, p.string ());
              ofs.close ();
            }
            catch (const io_error& e)
            {
              fail << "unable to write to " << p << ": " << e;
            }
          }
          else
            serialize (cout, "stdout");
        }
        else
        {
          // Merge complements/prerequisites from all fragments "upgrading"
          // prerequisites to complements and preferring locations from the
          // latest fragments.
          //
          vector<repository_manifest> rms;

          for (rep_fetch_data::fragment& fr: rfd.fragments)
          {
            for (repository_manifest& rm: fr.repositories)
            {
              if (rm.effective_role () == repository_role::base)
                continue;

              repository_location l (rm.location, rl); // Complete.

              auto i (find_if (rms.begin (), rms.end (),
                               [&l] (const repository_manifest& i)
                               {
                                 return i.location.canonical_name () ==
                                   l.canonical_name ();
                               }));

              if (i == rms.end ())
              {
                rm.location = move (l);
                rms.push_back (move (rm));
              }
              else
              {
                if (rm.effective_role () == repository_role::complement)
                  i->role = rm.effective_role ();

                i->location = move (l);
              }
            }
          }

          for (const repository_manifest& rm: rms)
          {
            repository_role rr (rm.effective_role ());

            const repository_location& l (rm.location);
            const string& n (l.canonical_name ());

            switch (rr)
            {
            case repository_role::complement:
              cout << "complement " << n << " " << l << endl;
              break;
            case repository_role::prerequisite:
              cout << "prerequisite " << n << " " << l << endl;
              break;
            case repository_role::base:
              assert (false);
            }
          }
        }
      }

      // Packages.
      //
      if (all || o.packages ())
      {
        if (o.manifest ())
        {
          // Merge package manifest lists, adding the fragment.
          //
          vector<package_manifest> pms;

          for (rep_fetch_data::fragment& fr: rfd.fragments)
          {
            for (package_manifest& pm: fr.packages)
            {
              if (!fr.id.empty ())
                pm.fragment = fr.id;

              pms.push_back (move (pm));
            }
          }

          auto serialize = [&pms] (ostream& os, const string& name)
          {
            // Note: serializing without any extra package_manifests info.
            //
            manifest_serializer s (os, name);

            for (const package_manifest& pm: pms)
              pm.serialize (s);

            s.next ("", ""); // End of stream.
          };

          if (o.packages_file_specified ())
          {
            const path& p (o.packages_file ());

            try
            {
              // Let's set the binary mode not to litter the manifest file
              // with the carriage return characters on Windows.
              //
              ofdstream ofs (p, fdopen_mode::binary);
              serialize (ofs, p.string ());
              ofs.close ();
            }
            catch (const io_error& e)
            {
              fail << "unable to write to " << p << ": " << e;
            }
          }
          else
            serialize (cout, "stdout");
        }
        else
        {
          // Merge packages from all the fragments.
          //
          vector<package_manifest> pms;

          for (rep_fetch_data::fragment& fr: rfd.fragments)
          {
            for (package_manifest& pm: fr.packages)
            {
              auto i (find_if (pms.begin (), pms.end (),
                               [&pm] (const package_manifest& i)
                               {
                                 return i.name == pm.name &&
                                   i.version == pm.version;
                               }));

              if (i == pms.end ())
                pms.push_back (move (pm));
            }
          }

          // Separate package list from the general repository info.
          //
          cout << endl;

          for (const package_manifest& pm: pms)
            cout << pm.name << "/" << pm.version << endl;
        }
      }
    }
    catch (const manifest_serialization& e)
    {
      fail << "unable to serialize manifest: " << e.description;
    }
    catch (const io_error&)
    {
      fail << "unable to write to stdout";
    }

    return 0;
  }

  default_options_files
  options_files (const char*, const rep_info_options& o, const strings&)
  {
    // NOTE: remember to update the documentation if changing anything here.

    // bpkg.options
    // bpkg-rep-info.options

    // If bpkg-rep-info operates in the configuration directory, then use it
    // as a search start directory.
    //
    optional<dir_path> start;

    // Let rep_info() complain later for invalid configuration directory.
    //
    try
    {
      dir_path d;

      if (o.directory_specified ())
        d = dir_path (o.directory ());
      else if (exists (bpkg_dir))
        d = current_dir;

      if (!d.empty ())
        start = move (normalize (d, "configuration"));
    }
    catch (const invalid_path&) {}

    return default_options_files {
      {path ("bpkg.options"), path ("bpkg-rep-info.options")},
      move (start)};
  }

  rep_info_options
  merge_options (const default_options<rep_info_options>& defs,
                 const rep_info_options& cmd)
  {
    // NOTE: remember to update the documentation if changing anything here.

    return merge_default_options (
      defs,
      cmd,
      [] (const default_options_entry<rep_info_options>& e,
          const rep_info_options&)
      {
        if (e.options.directory_specified ())
          fail (e.file) << "--directory|-d in default options file";
      });
  }
}
