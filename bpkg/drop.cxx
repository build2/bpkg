// file      : bpkg/drop.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/drop>

#include <map>
#include <iostream>   // cout

#include <butl/utility> // reverse_iterate()

#include <bpkg/types>
#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/utility>
#include <bpkg/database>
#include <bpkg/diagnostics>
#include <bpkg/satisfaction>
#include <bpkg/manifest-utility>

#include <bpkg/common-options>

#include <bpkg/pkg-purge>
#include <bpkg/pkg-disfigure>

using namespace std;
using namespace butl;

namespace bpkg
{
  using package_map = map<string, shared_ptr<selected_package>>;

  static void
  collect_dependent (database& db,
                     package_map& m,
                     const shared_ptr<selected_package>& p,
                     bool w)
  {
    using query = query<package_dependent>;

    bool found (false);

    for (auto& pd: db.query<package_dependent> (query::name == p->name))
    {
      string& dn (pd.name);

      if (m.find (dn) == m.end ())
      {
        shared_ptr<selected_package> dp (db.load<selected_package> (dn));
        m.emplace (move (dn), dp);

        collect_dependent (db, m, dp, w);

        if (w)
          warn << "dependent package " << dp->name << " to be dropped as well";

        found = true;
      }
    }

    if (w && found)
      info << "because dropping " << p->name;
  }

  int
  drop (const drop_options& o, cli::scanner& args)
  {
    tracer trace ("drop");

    const dir_path& c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help drop' for more information";

    database db (open (c, trace));

    // Note that the session spans all our transactions. The idea here is
    // that selected_package objects in the satisfied_packages list below
    // will be cached in this session. When subsequent transactions modify
    // any of these objects, they will modify the cached instance, which
    // means our list will always "see" their updated state.
    //
    // @@ Revise.
    //
    session s;

    // Assemble the list of packages we will need to drop. Comparing pointers
    // is valid because of the session above.
    //
    package_map pkgs;
    vector<string> names;
    {
      transaction t (db.begin ());

      // The first step is to load all the packages specified by the user.
      //
      while (args.more ())
      {
        string n (args.next ());
        level4 ([&]{trace << "package " << n;});

        shared_ptr<selected_package> p (db.find<selected_package> (n));

        if (p == nullptr)
          fail << "package " << n << " does not exist in configuration " << c;

        if (p->state == package_state::broken)
          fail << "unable to drop broken package " << n <<
            info << "use 'pkg-purge --force' to remove";

        if (pkgs.emplace (n, move (p)).second)
          names.push_back (move (n));
      }

      // The next step is to see if there are any dependents that are not
      // already on the list. We will have to drop those as well.
      //
      for (const string& n: names)
      {
        const shared_ptr<selected_package>& p (pkgs[n]);

        // Unconfigured package cannot have any dependents.
        //
        if (p->state != package_state::configured)
          continue;

        collect_dependent (db, pkgs, p, !o.drop_dependent ());
      }

      // If we've found dependents, ask the user to confirm.
      //
      if (!o.drop_dependent () && names.size () != pkgs.size ())
      {
        if (o.yes ())
          fail << "refusing to drop dependent packages with just --yes" <<
            info << "specify --drop-dependent to confirm";

        if (!yn_prompt ("drop dependent packages? [y/N]", 'n'))
          return 1;
      }

      t.commit ();
    }

    return 0;
  }
}
