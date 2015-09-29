// file      : bpkg/pkg-status.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-status>

#include <iostream>   // cout
#include <functional> // function

#include <bpkg/types>
#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/utility>
#include <bpkg/database>
#include <bpkg/diagnostics>
#include <bpkg/manifest-utility>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  pkg_status (const pkg_status_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_status");

    const dir_path& c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-status' for more information";

    string n (args.next ());

    version v;
    if (args.more ())
      v = parse_version (args.next ());

    database db (open (c, trace));
    transaction t (db.begin ());
    session s;

    level4 ([&]{trace << "package " << n << "; version " << v;});

    // First search in the packages that already exist in this configuration.
    //
    shared_ptr<package> p;
    {
      using query = query<package>;
      query q (query::name == n);

      if (!v.empty ())
        q = q && query::version == v;

      p = db.query_one<package> (q);
    }

    // Now look for available packages. If the user specified the version
    // explicitly and we found the corresponding existing package, then
    // no need to look for it in available packages.
    //
    vector<shared_ptr<available_package>> aps;
    if (p == nullptr || v.empty ())
    {
      using query = query<available_package>;

      query q (query::id.name == n);

      // If we found an existing package, then only look for versions
      // greater than what already exists.
      //
      if (p != nullptr)
        q = q && query::id.version > p->version;
      else if (!v.empty ())
        //
        // Otherwise, if the user specified the version, then only look for
        // that specific version.
        //
        q = q && query::id.version == v;

      q += order_by_version_desc (query::id.version);

      // Only consider packages that are in repositories that were
      // explicitly added to the configuration and their complements,
      // transitively. While we could maybe come up with a (barely
      // comprehensible) view/query to achieve this, doing it on the
      // "client side" is definitely more straightforward.
      //
      shared_ptr<repository> root (db.load<repository> (""));

      for (shared_ptr<available_package> ap:
             pointer_result (db.query<available_package> (q)))
      {
        function<bool (const shared_ptr<repository>&)> find =
          [&ap, &find](const shared_ptr<repository>& r) -> bool
        {
          const auto& cs (r->complements);

          for (const package_location& pl: ap->locations)
          {
            // First check all the complements without loading them.
            //
            if (cs.find (pl.repository) != cs.end ())
              return true;

            // If not found, then load the complements and check them
            // recursively.
            //
            for (lazy_shared_ptr<repository> cr: cs)
            {
              if (find (cr.load ()))
                return true;
            }
          }

          return false;
        };

        level4 ([&]{trace << "available " << ap->version;});

        if (find (root))
          aps.push_back (ap);
      }
    }

    t.commit ();

    bool found (false);

    if (p != nullptr)
    {
      cout << p->state;

      // Also print the version of the package unless the user specified it.
      //
      if (v.empty ())
        cout << " " << p->version;

      found = true;
    }

    if (!aps.empty ())
    {
      cout << (found ? "; " : "") << "available";

      // If the user specified the version, then there will be only one
      // entry.
      //
      if (v.empty ())
      {
        for (shared_ptr<available_package> ap: aps)
          cout << ' ' << ap->version;
      }

      found = true;
    }

    if (!found)
      cout << "unknown";

    cout << endl;
  }
}
