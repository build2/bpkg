// file      : bpkg/pkg-status.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-status.hxx>

#include <iostream>   // cout

#include <libbutl/json/serializer.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/package-query.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  struct package
  {
    database&                    pdb;        // Package database.
    database&                    rdb;        // Repository info database.
    package_name                 name;
    bpkg::version                version;    // Empty if unspecified.
    shared_ptr<selected_package> selected;   // NULL if none selected.
    optional<version_constraint> constraint; // Version constraint, if any.
  };
  using packages = vector<package>;

  struct available_package_status
  {
    shared_ptr<available_package> package;

    // Can only be built as a dependency.
    //
    // True if this package version doesn't belong to the repositories that
    // were explicitly added to the configuration and their complements,
    // recursively.
    //
    bool dependency;
  };

  class available_package_statuses: public vector<available_package_status>
  {
  public:
    // Empty if the package is not available from the system. Can be `?`.
    //
    string system_package_version;

    // Can only be built as a dependency.
    //
    // True if there are no package versions available from the repositories
    // that were explicitly added to the configuration and their complements,
    // recursively.
    //
    bool dependency = true;
  };

  static available_package_statuses
  pkg_statuses (const pkg_status_options& o, const package& p)
  {
    database& rdb (p.rdb);
    const shared_ptr<selected_package>& s (p.selected);

    available_package_statuses r;

    bool known (false);

    shared_ptr<repository_fragment> root (
      rdb.load<repository_fragment> (""));

    using query = query<available_package>;

    query q (query::id.name == p.name);
    {
      auto qr (rdb.query<available_package> (q));
      known = !qr.empty ();
      r.dependency = (filter_one (root, move (qr)).first == nullptr);
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

      for (shared_ptr<available_package> ap:
             pointer_result (rdb.query<available_package> (q)))
      {
        bool dependency (filter (root, ap) == nullptr);
        r.push_back (available_package_status {move (ap), dependency});
      }

      // The idea is that in the future we will try to auto-discover a system
      // version. For now we just say "maybe available from the system" even
      // if the version was specified by the user. We will later compare it if
      // the user did specify the version.
      //
      if (o.system ())
        r.system_package_version = '?';

      // Get rid of stubs.
      //
      for (auto i (r.begin ()); i != r.end (); ++i)
      {
        if (i->package->stub ())
        {
          // All the rest are stubs so bail out.
          //
          r.erase (i, r.end ());
          break;
        }
      }
    }

    return r;
  }

  static packages
  pkg_prerequisites (const shared_ptr<selected_package>& s, database& rdb)
  {
    packages r;
    for (const auto& pair: s->prerequisites)
    {
      shared_ptr<selected_package> d (pair.first.load ());
      database& db (pair.first.database ());
      const optional<version_constraint>& c (pair.second.constraint);
      r.push_back (package {db, rdb, d->name, version (), move (d), c});
    }
    return r;
  }

  static void
  pkg_status_lines (const pkg_status_options& o,
                    const packages& pkgs,
                    string& indent,
                    bool recursive,
                    bool immediate)
  {
    tracer trace ("pkg_status_lines");

    for (const package& p: pkgs)
    {
      l4 ([&]{trace << "package " << p.name << "; version " << p.version;});

      available_package_statuses ps (pkg_statuses (o, p));

      cout << indent;

      // Selected.
      //
      const shared_ptr<selected_package>& s (p.selected);

      // Hold package status.
      //
      if (s != nullptr)
      {
        if (s->hold_package && !o.no_hold () && !o.no_hold_package ())
          cout << '!';
      }

      // If the package name is selected, then print its exact spelling.
      //
      cout << (s != nullptr ? s->name : p.name) << p.pdb;

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
      if (!ps.empty () || !ps.system_package_version.empty ())
      {
        cout << (s != nullptr ? " " : "") << "available";

        for (const available_package_status& a: ps)
        {
          const version& v (a.package->version);

          // Show the currently selected version in parenthesis.
          //
          bool cur (s != nullptr && v == s->version);

          cout << ' '
               << (cur ? "(" : a.dependency ? "[" : "")
               << v
               << (cur ? ")" : a.dependency ? "]" : "");
        }

        if (!ps.system_package_version.empty ())
          cout << ' '
               << (ps.dependency ? "[" : "")
               << "sys:" << ps.system_package_version
               << (ps.dependency ? "]" : "");
      }
      //
      // Unknown.
      //
      else if (s == nullptr)
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
        // Let's propagate the repository information source database from the
        // dependent to its prerequisites.
        //
        if (s != nullptr)
        {
          packages dpkgs (pkg_prerequisites (s, p.rdb));

          if (!dpkgs.empty ())
          {
            indent += "  ";
            pkg_status_lines (o, dpkgs, indent, recursive, false /* immediate */);
            indent.resize (indent.size () - 2);
          }
        }
      }
    }
  }

  static void
  pkg_status_json (const pkg_status_options& o,
                   const packages& pkgs,
                   json::stream_serializer& ss,
                   bool recursive,
                   bool immediate)
  {
    tracer trace ("pkg_status_json");

    ss.begin_array ();

    for (const package& p: pkgs)
    {
      l4 ([&]{trace << "package " << p.name << "; version " << p.version;});

      available_package_statuses ps (pkg_statuses (o, p));

      const shared_ptr<selected_package>& s (p.selected);

      // Note that we won't check some values for being valid UTF-8 (package
      // names, etc), since their characters belong to even stricter character
      // sets.
      //
      ss.begin_object ();

      // If the package name is selected, then print its exact spelling.
      //
      ss.member ("name",
                 (s != nullptr ? s->name : p.name).string (),
                 false /* check */);

      if (!p.pdb.string.empty ())
        ss.member ("configuration", p.pdb.string);

      if (o.constraint () && p.constraint)
        ss.member ("constraint", p.constraint->string (), false /* check */);

      // Selected.
      //
      if (s != nullptr)
      {
        ss.member ("status", to_string (s->state), false /* check */);

        if (s->substate != package_substate::none)
          ss.member ("sub_status", to_string (s->substate), false /* check */);

        ss.member ("version", s->version_string (), false /* check */);

        if (s->hold_package)
          ss.member ("hold_package", true);

        if (s->hold_version)
          ss.member ("hold_version", true);
      }

      // Available.
      //
      if (!ps.empty () || !ps.system_package_version.empty ())
      {
        if (s == nullptr)
        {
          ss.member ("status", "available", false /* check */);

          // Print the user's version if specified.
          //
          if (!p.version.empty ())
            ss.member ("version", p.version.string (), false /* check */);
        }

        // Print the list of available versions, unless a specific available
        // version is already printed.
        //
        if (s != nullptr || p.version.empty ())
        {
          ss.member_name ("available_versions", false /* check */);

          // Serialize an available package version.
          //
          auto serialize = [&ss] (const string& v, bool s, bool d)
          {
            ss.begin_object ();

            ss.member ("version", v, false /* check */);

            if (s)
              ss.member ("system", s);

            if (d)
              ss.member ("dependency", d);

            ss.end_object ();
          };

          ss.begin_array ();

          for (const available_package_status& a: ps)
            serialize (a.package->version.string (),
                       false /* system */,
                       a.dependency);

          if (!ps.system_package_version.empty ())
            serialize (ps.system_package_version,
                       true /* system */,
                       ps.dependency);

          ss.end_array ();
        }
      }
      //
      // Unknown.
      //
      else if (s == nullptr)
      {
        ss.member ("status", "unknown", false /* check */);

        // Print the user's version if specified.
        //
        if (!p.version.empty ())
          ss.member ("version", p.version.string (), false /* check */);
      }

      if (recursive || immediate)
      {
        // Collect and recurse.
        //
        // Let's propagate the repository information source database from the
        // dependent to its prerequisites.
        //
        if (s != nullptr)
        {
          packages dpkgs (pkg_prerequisites (s, p.rdb));

          if (!dpkgs.empty ())
          {
            ss.member_name ("dependencies", false /* check */);
            pkg_status_json (o, dpkgs, ss, recursive, false /* immediate */);
          }
        }
      }

      ss.end_object ();
    }

    ss.end_array ();
  }

  int
  pkg_status (const pkg_status_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_status");

    if (o.immediate () && o.recursive ())
      fail << "both --immediate|-i and --recursive|-r specified";

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    database db (c, trace, true /* pre_attach */);
    transaction t (db);
    session s;

    // Let's use as repository information source the package database for the
    // held packages and the current database for the dependency packages.
    //
    // For the dependency packages we should probably use their dependent held
    // package configurations recursively, but feels a bit hairy at the
    // moment. So let's keep it simple for now. @@ TODO.
    //
    packages pkgs;
    {
      using query = query<selected_package>;

      if (args.more ())
      {
        while (args.more ())
        {
          const char* arg (args.next ());

          package_name pn (parse_package_name (arg));
          version pv (parse_package_version (arg,
                                             false /* allow_wildcard */,
                                             version::none));

          query q (query::name == pn);

          if (!pv.empty ())
            q = q && compare_version_eq (query::version,
                                         canonical_version (pv),
                                         pv.revision.has_value (),
                                         false /* iteration */);

          // Search in the packages that already exist in this and all the
          // dependency configurations.
          //
          bool found (false);
          for (database& ldb: db.dependency_configs ())
          {
            shared_ptr<selected_package> sp (
              ldb.query_one<selected_package> (q));

            if (sp != nullptr)
            {
              pkgs.push_back (package {ldb,
                                       sp->hold_package ? ldb : db,
                                       pn,
                                       pv,
                                       move (sp),
                                       nullopt /* constraint */});
              found = true;
            }
          }

          if (!found)
          {
            pkgs.push_back (package {db,
                                     db,
                                     move (pn),
                                     move (pv),
                                     nullptr  /* selected */,
                                     nullopt  /* constraint */});
          }
        }
      }
      else
      {
        // Find held/all packages in this and, if --link specified, all the
        // dependency configurations.
        //
        query q;

        if (!o.all ())
          q = query::hold_package;

        for (database& ldb: db.dependency_configs ())
        {
          for (shared_ptr<selected_package> s:
                 pointer_result (
                   ldb.query<selected_package> (q)))
          {
            pkgs.push_back (package {ldb,
                                     s->hold_package ? ldb : db,
                                     s->name,
                                     version (),
                                     move (s),
                                     nullopt /* constraint */});
          }

          if (!o.link ())
            break;
        }

        if (pkgs.empty ())
        {
          if (o.all ())
            info << "no packages in the configuration";
          else
            info << "no held packages in the configuration" <<
              info << "use --all|-a to see status of all packages";

          return 0;
        }
      }
    }

    switch (o.stdout_format ())
    {
    case stdout_format::lines:
      {
        string indent;
        pkg_status_lines (o, pkgs, indent, o.recursive (), o.immediate ());
        break;
      }
    case stdout_format::json:
      {
        json::stream_serializer s (cout);
        pkg_status_json (o, pkgs, s, o.recursive (), o.immediate ());
        cout << endl;
        break;
      }
    }

    t.commit ();
    return 0;
  }
}
