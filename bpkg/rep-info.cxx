// file      : bpkg/rep-info.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-info>

#include <iostream>  // cout
#include <stdexcept> // invalid_argument

#include <bpkg/manifest>
#include <bpkg/manifest-serializer>

#include <bpkg/fetch>
#include <bpkg/types>
#include <bpkg/utility>
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
    // unknown manifest entries unless we are dumping them.
    //
    repository_manifests rms (fetch_repositories (o, rl, !o.manifest ()));
    package_manifests pms (fetch_packages (o, rl, !o.manifest ()));

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
