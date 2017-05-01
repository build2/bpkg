// file      : bpkg/pkg-disfigure.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-disfigure.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  pkg_disfigure (const dir_path& c,
                 const common_options& o,
                 transaction& t,
                 const shared_ptr<selected_package>& p)
  {
    assert (p->state == package_state::configured ||
            p->state == package_state::broken);

    tracer trace ("pkg_disfigure");

    l4 ([&]{trace << *p;});

    database& db (t.database ());
    tracer_guard tg (db, trace);

    // Check that we have no dependents.
    //
    if (p->state == package_state::configured)
    {
      using query = query<package_dependent>;

      auto r (db.query<package_dependent> (query::name == p->name));

      if (!r.empty ())
      {
        diag_record dr;
        dr << fail << "package " << p->name << " still has dependencies:";

        for (const package_dependent& pd: r)
        {
          dr << info << "package " << pd.name;

          if (pd.constraint)
            dr << " on " << p->name << " " << *pd.constraint;
        }
      }
    }

    if (p->substate == package_substate::system)
    {
      db.erase (p);
      t.commit ();

      p->state = package_state::transient;
      p->substate = package_substate::none;

      return;
    }

    // Since we are no longer configured, clear the prerequisites list.
    //
    p->prerequisites.clear ();

    // Calculate package's src_root and out_root.
    //
    assert (p->src_root); // Must be set since unpacked.
    assert (p->out_root); // Must be set since configured.

    dir_path src_root (p->src_root->absolute ()
                       ? *p->src_root
                       : c / *p->src_root);
    dir_path out_root (c / *p->out_root); // Always relative.

    l4 ([&]{trace << "src_root: " << src_root << ", "
                  << "out_root: " << out_root;});

    // Form the buildspec.
    //
    string bspec;

    // Use path representation to get canonical trailing slash.
    //
    const string& rep (out_root.representation ());

    if (p->state == package_state::configured)
    {
      bspec = "clean('" + rep + "') " "disfigure('" + rep + "')";
    }
    else
    {
      // Why do we need to specify src_root? While it's unnecessary
      // for a completely configured package, here we disfigure a
      // partially configured one.
      //
      if (src_root == out_root)
        bspec = "disfigure('" + rep + "')";
      else
        bspec = "disfigure('" + src_root.representation () + "'@'" +
          rep + "')";
    }

    l4 ([&]{trace << "buildspec: " << bspec;});

    // Disfigure.
    //
    try
    {
      if (exists (out_root))
        run_b (o, c, bspec, true); // Run quiet.

      // Make sure the out directory is gone unless it is the same as src.
      //
      if (out_root != src_root && exists (out_root))
        fail << "package output directory " << out_root << " still exists";
    }
    catch (const failed&)
    {
      // If we failed to disfigure the package, set it to the broken
      // state. The user can then try to clean things up with pkg-purge.
      //
      p->state = package_state::broken;
      db.update (p);
      t.commit ();

      info << "package " << p->name << " is now broken; "
           << "use 'pkg-purge' to remove";
      throw;
    }

    p->out_root = nullopt;
    p->state = package_state::unpacked;

    db.update (p);
    t.commit ();
  }

  int
  pkg_disfigure (const pkg_disfigure_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_disfigure");

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-disfigure' for more information";

    string n (args.next ());

    database db (open (c, trace));
    transaction t (db.begin ());

    shared_ptr<selected_package> p (db.find<selected_package> (n));

    if (p == nullptr)
      fail << "package " << n << " does not exist in configuration " << c;

    if (p->state != package_state::configured)
      fail << "package " << n << " is " << p->state <<
        info << "expected it to be configured";

    pkg_disfigure (c, o, t, p); // Commits the transaction.

    assert (p->state == package_state::unpacked ||
            p->state == package_state::transient);

    if (verb)
      text << (p->state == package_state::transient
               ? "purged "
               : "disfigured ") << *p;

    return 0;
  }
}
