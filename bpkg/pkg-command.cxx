// file      : bpkg/pkg-command.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-command>

#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/database>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  pkg_command (const string& cmd,
               const dir_path& c,
               const common_options& o,
               const strings& cvars,
               const vector<pkg_command_vars>& ps)
  {
    tracer trace ("pkg_command");

    l4 ([&]{trace << "command: " << cmd;});

    // This one is a bit tricky: we can only update all the packages at once if
    // they don't have any package-specific variables. But let's try to handle
    // this with the same logic (being clever again).
    //
    string bspec;

    auto run =
      [&trace, &c, &o, &cvars, &bspec] ( const strings& vars = strings ())
    {
      if (!bspec.empty ())
      {
        bspec += ')';
        l4 ([&]{trace << "buildspec: " << bspec;});
        run_b (o, c, bspec, false, vars, cvars);
        bspec.clear ();
      }
    };

    for (const pkg_command_vars& pv: ps)
    {
      if (!pv.vars.empty ())
        run (); // Run previously collected packages.

      if (bspec.empty ())
        bspec = cmd + '(';

      const shared_ptr<selected_package>& p (pv.pkg);

      assert (p->state == package_state::configured);
      assert (p->out_root); // Should be present since configured.

      dir_path out_root (c / *p->out_root); // Always relative.
      l4 ([&]{trace << p->name << " out_root: " << out_root;});

      if (bspec.back () != '(')
        bspec += ' ';

      bspec += '\'';
      bspec += out_root.string ();
      bspec += "/'";

      if (!pv.vars.empty ())
        run (pv.vars); // Run this package.
    }

    run ();
  }

  int
  pkg_command (const string& cmd,
               const configuration_options& o,
               cli::scanner& args)
  {
    tracer trace ("pkg_command");

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    // First read the common variables.
    //
    auto read_vars = [&args](strings& v)
      {
        for (; args.more (); args.next ())
        {
          string a (args.peek ());

          if (a.find ('=') == string::npos)
            break;

          v.push_back (move (a));
        }
      };

    strings cvars;
    read_vars (cvars);

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-" << cmd << "' for more information";

    vector<pkg_command_vars> ps;
    {
      database db (open (c, trace));
      transaction t (db.begin ());

      while (args.more ())
      {
        string n (args.next ());
        shared_ptr<selected_package> p (db.find<selected_package> (n));

        if (p == nullptr)
          fail << "package " << n << " does not exist in configuration " << c;

        if (p->state != package_state::configured)
          fail << "package " << n << " is " << p->state <<
            info << "expected it to be configured";

        l4 ([&]{trace << p->name << " " << p->version;});

        // Read package-specific variables.
        //
        strings vars;
        read_vars (vars);

        ps.push_back (pkg_command_vars {move (p), move (vars)});
      }

      t.commit ();
    }

    pkg_command (cmd, c, o, cvars, ps);

    if (verb)
    {
      for (const pkg_command_vars& pv: ps)
        text << cmd << (cmd.back () != 'e' ? "ed " : "d ")
             << pv.pkg->name << " " << pv.pkg->version;
    }

    return 0;
  }
}
