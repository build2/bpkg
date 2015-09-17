// file      : bpkg/pkg-configure.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-configure>

#include <bpkg/types>
#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/utility>
#include <bpkg/database>
#include <bpkg/diagnostics>

#include <bpkg/pkg-disfigure>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
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

    shared_ptr<package> p (db.find<package> (n));

    if (p == nullptr)
      fail << "package " << n << " does not exist in configuration " << c;

    if (p->state != state::unpacked)
      fail << "package " << n << " is " << p->state <<
        info << "expected it to be unpacked";

    level4 ([&]{trace << p->name << " " << p->version;});

    // Calculate package's src_root and out_root.
    //
    assert (p->src_root); // Must be set since unpacked.

    dir_path src_root (p->src_root->absolute ()
                       ? *p->src_root
                       : c / *p->src_root);
    dir_path out_root (c / dir_path (p->name + "-" + p->version.string ()));

    level4 ([&]{trace << "src_root: " << src_root << ", "
                      << "out_root: " << out_root;});

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
      run_b (bspec);
    }
    catch (const failed&)
    {
      // If we failed to configure the package, make sure we revert
      // it back to the unpacked state by running disfigure (it is
      // valid to run disfigure on an un-configured build). And if
      // disfigure fails as well, then the package will be set into
      // the broken state.
      //

      // Pretend we are configured.
      //
      p->out_root = out_root.leaf ();
      p->state = state::configured;

      pkg_disfigure (c, t, p); // Commits the transaction.
      throw;
    }

    p->out_root = out_root.leaf ();
    p->state = state::configured;

    db.update (p);
    t.commit ();

    if (verb)
      text << "configured " << p->name << " " << p->version;
  }
}
