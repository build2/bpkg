// file      : bpkg/pkg-status.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-status.hxx>

#include <iostream>   // cout

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  struct package
  {
    string                       name;
    bpkg::version                version;  // Empty if unspecified.
    shared_ptr<selected_package> selected; // NULL if none selected.
  };
  using packages = vector<package>;

  int
  pkg_status (const pkg_status_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_status");

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    database db (open (c, trace));
    transaction t (db.begin ());
    session s;

    packages pkgs;
    bool single (false); // True if single package specified by the user.
    {
      using query = query<selected_package>;

      if (args.more ())
      {
        while (args.more ())
        {
          const char* arg (args.next ());
          package p {parse_package_name (arg),
                     parse_package_version (arg),
                     nullptr};

          // Search in the packages that already exist in this configuration.
          //
          {
            query q (query::name == p.name);

            if (!p.version.empty ())
              q = q && compare_version_eq (query::version,
                                           p.version,
                                           p.version.revision != 0);

            p.selected = db.query_one<selected_package> (q);
          }

          pkgs.push_back (move (p));
        }

        single = (pkgs.size () == 1);
      }
      else
      {
        // Find all held packages.
        //
        for (shared_ptr<selected_package> s:
               pointer_result (
                 db.query<selected_package> (query::hold_package)))
        {
          pkgs.push_back (package {s->name, version (), move (s)});
        }

        if (pkgs.empty ())
        {
          info << "no held packages in the configuration";
          return 0;
        }
      }
    }

    for (const package& p: pkgs)
    {
      l4 ([&]{trace << "package " << p.name << "; version " << p.version;});

      // Look for available packages.
      //
      bool available; // At least one vailable package (stub or not).
      vector<shared_ptr<available_package>> apkgs;
      {
        shared_ptr<repository> rep (db.load<repository> ("")); // Root.

        using query = query<available_package>;

        query q (query::id.name == p.name);

        available =
          filter_one (rep, db.query<available_package> (q)).first != nullptr;

        if (available)
        {
          // If the user specified the version, then only look for that
          // specific version (we still do it since there might be other
          // revisions).
          //
          if (!p.version.empty ())
            q = q && compare_version_eq (query::id.version,
                                         p.version,
                                         p.version.revision != 0);

          // And if we found an existing package, then only look for versions
          // greater than what already exists. Note that for a system wildcard
          // version we will always show all available versions (since it's
          // 0).
          //
          if (p.selected != nullptr)
            q = q && query::id.version > p.selected->version;

          q += order_by_version_desc (query::id.version);

          // Only consider packages that are in repositories that were
          // explicitly added to the configuration and their complements,
          // recursively.
          //
          apkgs = filter (rep, db.query<available_package> (q));
        }
      }

      // Suppress printing the package name if there is only one and it was
      // specified by the user.
      //
      if (!single)
      {
        cout << p.name;

        if (!p.version.empty ())
          cout << '/' << p.version;

        cout << ": ";
      }

      bool found (false);

      if (const shared_ptr<selected_package>& s = p.selected)
      {
        cout << s->state;

        if (s->substate != package_substate::none)
          cout << ',' << s->substate;

        // Also print the version of the package unless the user specified it.
        //
        if (p.version != s->version)
          cout << ' ' << s->version_string ();

        if (s->hold_package)
          cout << " hold_package";

        if (s->hold_version)
          cout << " hold_version";

        found = true;
      }

      if (available)
      {
        cout << (found ? "; " : "") << "available";

        // The idea is that in the future we will try to auto-discover a
        // system version and then print that. For now we just say "maybe
        // available from the system" but only if no version was specified by
        // the user. We will later compare it if the user did specify the
        // version.
        //
        bool sys (p.version.empty ());

        if (!apkgs.empty ())
        {
          // If the user specified the version, then there might only be one
          // entry in which case it is useless to repeat it. But we do want
          // to print it if there is also a system one.
          //
          if (sys                          ||
              p.version.empty ()           ||
              apkgs.size () > 1            ||
              p.version != apkgs[0]->version)
          {
            for (const shared_ptr<available_package>& a: apkgs)
            {
              if (a->stub ())
                break; // All the rest are stubs so bail out.

              cout << ' ' << a->version;
            }
          }
        }

        if (sys)
          cout << " sys:?";

        found = true;
      }

      if (!found)
        cout << "unknown";

      cout << endl;
    }

    t.commit ();
    return 0;
  }
}
