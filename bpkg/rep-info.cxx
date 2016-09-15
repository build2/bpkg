// file      : bpkg/rep-info.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-info>

#include <iostream>  // cout

#include <butl/sha256> // sha256_to_fingerprint()

#include <bpkg/manifest>
#include <bpkg/manifest-serializer>

#include <bpkg/auth>
#include <bpkg/fetch>
#include <bpkg/package>
#include <bpkg/diagnostics>
#include <bpkg/manifest-utility>

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

    repository_location rl (parse_location (args.next ()));

    // Fetch everything we will need before printing anything. Ignore
    // unknown manifest entries unless we are dumping them. First fetch
    // the repositories list and authenticate the base's certificate.
    //
    pair<repository_manifests, string/*checksum*/> rmc (
      fetch_repositories (o, rl, !o.manifest ()));

    repository_manifests& rms (rmc.first);

    bool a (o.auth () != auth::none &&
            (o.auth () == auth::all || rl.remote ()));

    const optional<string> cert_pem (rms.back ().certificate);
    shared_ptr<const certificate> cert;

    if (a)
    {
      dir_path d (o.directory ());
      cert = authenticate_certificate (
        o,
        o.directory_specified () && d.empty () ? nullptr : &d,
        cert_pem,
        rl);

      a = !cert->dummy ();
    }

    // Now fetch the packages list and make sure it matches the repositories
    // we just fetched.
    //
    pair<package_manifests, string/*checksum*/> pmc (
      fetch_packages (o, rl, !o.manifest ()));

    package_manifests& pms (pmc.first);

    if (rmc.second != pms.sha256sum)
      fail << "repositories manifest file checksum mismatch for "
           << rl.canonical_name () <<
        info << "try again";

    if (a)
    {
      signature_manifest sm (fetch_signature (o, rl, true));

      if (sm.sha256sum != pmc.second)
        fail << "packages manifest file checksum mismatch for "
             << rl.canonical_name () <<
          info << "try again";

      dir_path d (o.directory ());
      assert (cert != nullptr);

      authenticate_repository (
        o,
        o.directory_specified () && d.empty () ? nullptr : &d,
        cert_pem,
        *cert,
        sm,
        rl);
    }

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
                 << sha256_to_fingerprint (cert->fingerprint) << endl;
        }
        else
        {
          // Print in the structured format if any of --cert-* options are
          // specified. Print empty lines for an unsigned repository.
          //
          if (o.cert_fingerprint ())
          {
            if (cert != nullptr)
              cout << sha256_to_fingerprint (cert->fingerprint);
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
          manifest_serializer s (cout, "STDOUT");
          rms.serialize (s);
        }
        else
        {
          for (const repository_manifest& rm: rms)
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
          manifest_serializer s (cout, "STDOUT");
          pms.serialize (s);
        }
        else
        {
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
      fail << "unable to write to STDOUT";
    }

    return 0;
  }
}
