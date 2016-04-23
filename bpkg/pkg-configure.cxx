// file      : bpkg/pkg-configure.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-configure>

#include <bpkg/types>
#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/utility>
#include <bpkg/database>
#include <bpkg/diagnostics>
#include <bpkg/satisfaction>

#include <bpkg/pkg-verify>
#include <bpkg/pkg-disfigure>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  pkg_configure (const dir_path& c,
                 const common_options& o,
                 transaction& t,
                 const shared_ptr<selected_package>& p,
                 const strings& vars)
  {
    tracer trace ("pkg_configure");

    assert (p->state == package_state::unpacked);
    assert (p->src_root); // Must be set since unpacked.

    database& db (t.database ());
    tracer_guard tg (db, trace);

    // Calculate package's src_root and out_root.
    //
    dir_path src_root (p->src_root->absolute ()
                       ? *p->src_root
                       : c / *p->src_root);
    dir_path out_root (c / dir_path (p->name + "-" + p->version.string ()));

    level4 ([&]{trace << "src_root: " << src_root << ", "
                      << "out_root: " << out_root;});

    // Verify all our prerequisites are configured and populate the
    // prerequisites list.
    //
    {
      assert (p->prerequisites.empty ());

      package_manifest m (pkg_verify (src_root, true));

      for (const dependency_alternatives& da: m.dependencies)
      {
        assert (!da.conditional); //@@ TODO

        bool satisfied (false);
        for (const dependency& d: da)
        {
          const string& n (d.name);

          if (shared_ptr<selected_package> dp = db.find<selected_package> (n))
          {
            if (dp->state != package_state::configured)
              continue;

            if (!satisfies (dp->version, d.constraint))
              continue;

            auto r (p->prerequisites.emplace (dp, d.constraint));

            // Currently we can only capture a single constraint, so if we
            // already have a dependency on this package and one constraint is
            // not a subset of the other, complain.
            //
            if (!r.second)
            {
              auto& c (r.first->second);

              bool s1 (satisfies (c, d.constraint));
              bool s2 (satisfies (d.constraint, c));

              if (!s1 && !s2)
                fail << "multiple dependencies on package " << n <<
                  info << n << " " << *c <<
                  info << n << " " << *d.constraint;

              if (s2 && !s1)
                c = d.constraint;
            }

            satisfied = true;
            break;
          }
        }

        if (!satisfied)
          fail << "no configured package satisfies dependency on " << da;
      }
    }

    // Form the buildspec.
    //
    string bspec;

    if (src_root == out_root)
      bspec = "configure(" + out_root.string () + "/)";
    else
      bspec = "configure(" +
        src_root.string () + "/@" +
        out_root.string () + "/)";

    level4 ([&]{trace << "buildspec: " << bspec;});

    // Configure.
    //
    try
    {
      run_b (o, bspec, true, vars); // Run quiet.
    }
    catch (const failed&)
    {
      // If we failed to configure the package, make sure we revert
      // it back to the unpacked state by running disfigure (it is
      // valid to run disfigure on an un-configured build). And if
      // disfigure fails as well, then the package will be set into
      // the broken state.
      //

      // Indicate to pkg_disfigure() we are partially configured.
      //
      p->out_root = out_root.leaf ();
      p->state = package_state::broken;

      pkg_disfigure (c, o, t, p); // Commits the transaction.
      throw;
    }

    p->out_root = out_root.leaf ();
    p->state = package_state::configured;

    db.update (p);
    t.commit ();
  }

  int
  pkg_configure (const pkg_configure_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_configure");

    const dir_path& c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    // Sort arguments into the package name and configuration variables.
    //
    string n;
    strings vars;

    while (args.more ())
    {
      string a (args.next ());

      if (a.find ('=') != string::npos)
        vars.push_back (move (a));
      else if (n.empty ())
        n = move (a);
      else
        fail << "unexpected argument '" << a << "'";
    }

    if (n.empty ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-configure' for more information";

    database db (open (c, trace));
    transaction t (db.begin ());
    session s;

    shared_ptr<selected_package> p (db.find<selected_package> (n));

    if (p == nullptr)
      fail << "package " << n << " does not exist in configuration " << c;

    if (p->state != package_state::unpacked)
      fail << "package " << n << " is " << p->state <<
        info << "expected it to be unpacked";

    level4 ([&]{trace << p->name << " " << p->version;});

    pkg_configure (c, o, t, p, vars); // Commits the transaction.

    if (verb)
      text << "configured " << p->name << " " << p->version;

    return 0;
  }
}
