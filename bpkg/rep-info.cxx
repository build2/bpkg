// file      : bpkg/rep-info.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-info>

#include <iostream>  // cout

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
    bool all (!o.name () && !o.repositories () && !o.packages ());

    try
    {
      cout.exceptions (ostream::badbit | ostream::failbit);

      if (all || o.name ())
        cout << rl.canonical_name () << " " << rl << endl;

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
          for (const package_manifest& pm: pms)
            cout << pm.name << " " << pm.version << endl;
        }
      }
    }
    catch (const manifest_serialization& e)
    {
      fail << "unable to serialize manifest: " << e.description;
    }
    catch (const ostream::failure&)
    {
      fail << "unable to write to STDOUT";
    }

    return 0;
  }
}
