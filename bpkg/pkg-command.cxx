// file      : bpkg/pkg-command.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-command>

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
  pkg_command (const string& cmd,
               const dir_path& c,
               const common_options& o,
               const shared_ptr<selected_package>& p)
  {
    tracer trace ("pkg_command");

    level4 ([&]{trace << "command: " << cmd;});

    assert (p->state == package_state::configured);
    assert (p->out_root); // Should be present since configured.

    dir_path out_root (c / *p->out_root); // Always relative.
    level4 ([&]{trace << "out_root: " << out_root;});

    // Form the buildspec.
    //
    string bspec (cmd + "(" + out_root.string () + "/)");
    level4 ([&]{trace << "buildspec: " << bspec;});

    run_b (o, bspec);
  }

  void
  pkg_command (const string& cmd,
               const dir_path& c,
               const common_options& o,
               const vector<shared_ptr<selected_package>>& ps)
  {
    tracer trace ("pkg_command");

    level4 ([&]{trace << "command: " << cmd;});

    // Form the buildspec.
    //
    string bspec (cmd + "(");

    for (const shared_ptr<selected_package>& p: ps)
    {
      assert (p->state == package_state::configured);
      assert (p->out_root); // Should be present since configured.

      dir_path out_root (c / *p->out_root); // Always relative.
      level4 ([&]{trace << p->name << " out_root: " << out_root;});

      if (bspec.back () != '(')
        bspec += ' ';
      bspec += out_root.string ();
      bspec += '/';
    }

    bspec += ')';

    level4 ([&]{trace << "buildspec: " << bspec;});

    run_b (o, bspec);
  }

  int
  pkg_command (const string& cmd,
               const configuration_options& o,
               cli::scanner& args)
  {
    tracer trace ("pkg_command");

    const dir_path& c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-" << cmd << "' for more information";

    vector<shared_ptr<selected_package>> ps;
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

        level4 ([&]{trace << p->name << " " << p->version;});
        ps.push_back (move (p));
      }

      t.commit ();
    }

    pkg_command (cmd, c, o, ps);

    if (verb)
    {
      for (const shared_ptr<selected_package>& p: ps)
        text << cmd << (cmd.back () != 'e' ? "ed " : "d ")
             << p->name << " " << p->version;
    }

    return 0;
  }
}
