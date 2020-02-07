// file      : bpkg/pkg-status.cxx -*- C++ -*-
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
    package_name                 name;
    bpkg::version                version;    // Empty if unspecified.
    shared_ptr<selected_package> selected;   // NULL if none selected.
    optional<version_constraint> constraint; // Version constraint, if any.
  };
  using packages = vector<package>;

  // If recursive or immediate is true, then print status for dependencies
  // indented by two spaces.
  //
  static void
  pkg_status (const pkg_status_options& o,
              database& db,
              const packages& pkgs,
              string& indent,
              bool recursive,
              bool immediate)
  {
    tracer trace ("pkg_status");

    for (const package& p: pkgs)
    {
      l4 ([&]{trace << "package " << p.name << "; version " << p.version;});

      // Can't be both.
      //
      assert (p.version.empty () || !p.constraint);

      const shared_ptr<selected_package>& s (p.selected);

      // Look for available packages.
      //
      // Some of them are only available to upgrade/downgrade as dependencies.
      //
      struct apkg
      {
        shared_ptr<available_package> package;
        bool build;
      };
      vector<apkg> apkgs;

      // A package with this name is known in available packages potentially
      // for build.
      //
      bool known (false);
      bool build (false);
      {
        shared_ptr<repository_fragment> root (
          db.load<repository_fragment> (""));

        using query = query<available_package>;

        query q (query::id.name == p.name);
        {
          auto r (db.query<available_package> (q));
          known = !r.empty ();
          build = filter_one (root, move (r)).first != nullptr;
        }

        if (known)
        {
          // If the user specified the version, then only look for that
          // specific version (we still do it since there might be other
          // revisions).
          //
          if (!p.version.empty ())
            q = q && compare_version_eq (query::id.version,
                                         canonical_version (p.version),
                                         p.version.revision.has_value (),
                                         false /* iteration */);

          // And if we found an existing package, then only look for versions
          // greater than to what already exists unless we were asked to show
          // old versions.
          //
          // Note that for a system wildcard version we will always show all
          // available versions (since it is 0).
          //
          if (s != nullptr && !o.old_available ())
            q = q && query::id.version > canonical_version (s->version);

          q += order_by_version_desc (query::id.version);

          // Packages that are in repositories that were explicitly added to
          // the configuration and their complements, recursively, are also
          // available to build.
          //
          for (shared_ptr<available_package> ap:
                 pointer_result (
                   db.query<available_package> (q)))
          {
            bool build (filter (root, ap));
            apkgs.push_back (apkg {move (ap), build});
          }
        }
      }

      cout << indent;

      // Selected.
      //

      // Hold package status.
      //
      if (s != nullptr)
      {
        if (s->hold_package && !o.no_hold () && !o.no_hold_package ())
          cout << '!';
      }

      // If the package name is selected, then print its exact spelling.
      //
      cout << (s != nullptr ? s->name : p.name);

      if (o.constraint () && p.constraint)
        cout << ' ' << *p.constraint;

      cout << ' ';

      if (s != nullptr)
      {
        cout << s->state;

        if (s->substate != package_substate::none)
          cout << ',' << s->substate;

        cout << ' ';

        if (s->hold_version && !o.no_hold () && !o.no_hold_version ())
          cout << '!';

        cout << s->version_string ();
      }

      // Available.
      //
      bool available (false);
      if (known)
      {
        // Available from the system.
        //
        // The idea is that in the future we will try to auto-discover a
        // system version and then print that. For now we just say "maybe
        // available from the system" even if the version was specified by
        // the user. We will later compare it if the user did specify the
        // version.
        //
        string sys;
        if (o.system ())
        {
          sys = "?";
          available = true;
        }

        // Get rid of stubs.
        //
        for (auto i (apkgs.begin ()); i != apkgs.end (); ++i)
        {
          if (i->package->stub ())
          {
            // All the rest are stubs so bail out.
            //
            apkgs.erase (i, apkgs.end ());
            break;
          }

          available = true;
        }

        if (available)
        {
          cout << (s != nullptr ? " " : "") << "available";

          for (const apkg& a: apkgs)
          {
            const version& v (a.package->version);

            // Show the currently selected version in parenthesis.
            //
            bool cur (s != nullptr &&  v == s->version);

            cout << ' '
                 << (cur ? "(" : a.build ? "" : "[")
                 << v
                 << (cur ? ")" : a.build ? "" : "]");
          }

          if (!sys.empty ())
            cout << ' '
                 << (build ? "" : "[")
                 << "sys:" << sys
                 << (build ? "" : "]");
        }
      }

      if (s == nullptr && !available)
      {
        cout << "unknown";

        // Print the user's version if specified.
        //
        if (!p.version.empty ())
          cout << ' ' << p.version;
      }

      cout << endl;

      if (recursive || immediate)
      {
        // Collect and recurse.
        //
        packages dpkgs;
        if (s != nullptr)
        {
          for (const auto& pair: s->prerequisites)
          {
            shared_ptr<selected_package> d (pair.first.load ());
            const optional<version_constraint>& c (pair.second);
            dpkgs.push_back (package {d->name, version (), move (d), c});
          }
        }

        if (!dpkgs.empty ())
        {
          indent += "  ";
          pkg_status (o,
                      db,
                      dpkgs,
                      indent,
                      recursive,
                      false /* immediate */);
          indent.resize (indent.size () - 2);
        }
      }
    }
  }

  int
  pkg_status (const pkg_status_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_status");

    if (o.immediate () && o.recursive ())
      fail << "both --immediate|-i and --recursive|-r specified";

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    database db (open (c, trace));
    transaction t (db);
    session s;

    packages pkgs;
    {
      using query = query<selected_package>;

      if (args.more ())
      {
        while (args.more ())
        {
          const char* arg (args.next ());
          package p {parse_package_name (arg),
                     parse_package_version (arg,
                                            false /* allow_wildcard */,
                                            false /* fold_zero_revision */),
                     nullptr  /* selected */,
                     nullopt  /* constraint */};

          // Search in the packages that already exist in this configuration.
          //
          {
            query q (query::name == p.name);

            if (!p.version.empty ())
              q = q && compare_version_eq (query::version,
                                           canonical_version (p.version),
                                           p.version.revision.has_value (),
                                           false /* iteration */);

            p.selected = db.query_one<selected_package> (q);
          }

          pkgs.push_back (move (p));
        }
      }
      else
      {
        // Find all held packages.
        //
        for (shared_ptr<selected_package> s:
               pointer_result (
                 db.query<selected_package> (query::hold_package)))
        {
          pkgs.push_back (package {s->name, version (), move (s), nullopt});
        }

        if (pkgs.empty ())
        {
          info << "no held packages in the configuration";
          return 0;
        }
      }
    }

    string indent;
    pkg_status (o, db, pkgs, indent, o.recursive (), o.immediate ());

    t.commit ();
    return 0;
  }
}
