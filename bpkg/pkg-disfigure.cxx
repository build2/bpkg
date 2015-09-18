// file      : bpkg/pkg-disfigure.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-disfigure>

#include <bpkg/types>
#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/utility>
#include <bpkg/database>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  pkg_disfigure (const dir_path& c,
                 transaction& t,
                 const shared_ptr<package>& p)
  {
    tracer trace ("pkg_disfigure");
    t.tracer (trace); // "Tail" call, never restored.

    database& db (t.database ());

    // Calculate package's src_root and out_root.
    //
    assert (p->src_root); // Must be set since unpacked.
    assert (p->out_root); // Must be set since configured.

    dir_path src_root (p->src_root->absolute ()
                       ? *p->src_root
                       : c / *p->src_root);
    dir_path out_root (c / *p->out_root); // Always relative.

    level4 ([&]{trace << "src_root: " << src_root << ", "
                      << "out_root: " << out_root;});

    // Form the buildspec.
    //
    string bspec;

    if (p->state != state::broken)
    {
      bspec = "clean(" + out_root.string () + "/) "
        "disfigure(" + out_root.string () + "/)";
    }
    else
    {
      // Why do we need to specify src_root? While it's unnecessary
      // for a completely configured package, here we disfigure a
      // partially configured one.
      //
      if (src_root == out_root)
        bspec = "disfigure(" + out_root.string () + "/)";
      else
        bspec = "disfigure(" +
          src_root.string () + "/@" +
          out_root.string () + "/)";
    }

    level4 ([&]{trace << "buildspec: " << bspec;});

    // Disfigure.
    //
    try
    {
      if (exists (out_root))
        run_b (bspec, true); // Run quiet.

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
      p->state = state::broken;
      db.update (p);
      t.commit ();

      info << "package " << p->name << " is now broken; "
           << "use 'pkg-purge' to remove";
      throw;
    }

    p->out_root = nullopt;
    p->state = state::unpacked;

    db.update (p);
    t.commit ();
  }

  void
  pkg_disfigure (const pkg_disfigure_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_disfigure");

    const dir_path& c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-disfigure' for more information";

    string n (args.next ());

    database db (open (c, trace));
    transaction t (db.begin ());

    shared_ptr<package> p (db.find<package> (n));

    if (p == nullptr)
      fail << "package " << n << " does not exist in configuration " << c;

    if (p->state != state::configured)
      fail << "package " << n << " is " << p->state <<
        info << "expected it to be configured";

    level4 ([&]{trace << p->name << " " << p->version;});

    pkg_disfigure (c, t, p); // Commits the transaction.

    if (verb)
      text << "disfigured " << p->name << " " << p->version;
  }
}
