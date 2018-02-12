// file      : bpkg/rep-info.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-info.hxx>

#include <iostream>  // cout

#include <libbutl/sha256.mxx>              // sha256_to_fingerprint()
#include <libbutl/manifest-serializer.mxx>

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

    repository_location rl (
      parse_location (args.next (),
                      o.type_specified ()
                      ? optional<repository_type> (o.type ())
                      : nullopt));

    // Fetch everything we will need before printing anything. Ignore
    // unknown manifest entries unless we are dumping them.
    //
    dir_path d (o.directory ());
    rep_fetch_data rfd (
      rep_fetch (o,
                 o.directory_specified () && d.empty () ? nullptr : &d,
                 rl,
                 !o.manifest () /* ignore_unknow */));

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
        const optional<string>& cert_pem (rfd.repositories.back ().certificate);

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
          // Note: serializing without any extra repository_manifests info.
          //
          manifest_serializer s (cout, "STDOUT");
          for (const repository_manifest& rm: rfd.repositories)
            rm.serialize (s);
          s.next ("", ""); // End of stream.
        }
        else
        {
          for (const repository_manifest& rm: rfd.repositories)
          {
            repository_role rr (rm.effective_role ());

            if (rr == repository_role::base)
              continue; // Entry for this repository.

            repository_location l (rm.location, rl); // Complete.
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
          // Note: serializing without any extra package_manifests info.
          //
          manifest_serializer s (cout, "STDOUT");
          for (const package_manifest& pm: rfd.packages)
            pm.serialize (s);
          s.next ("", ""); // End of stream.
        }
        else
        {
          // Separate package list from the general repository info.
          //
          cout << endl;

          for (const package_manifest& pm: rfd.packages)
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
      fail << "unable to write to STDOUT";
    }

    return 0;
  }
}
