// file      : bpkg/pkg-command.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
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
               const configuration_options& o,
               cli::scanner& args)
  {
    tracer trace ("pkg_command");
    level4 ([&]{trace << "command: " << cmd;});

    const dir_path& c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-" << cmd << "' for more information";

    string n (args.next ());

    database db (open (c, trace));

    transaction t (db.begin ());
    shared_ptr<package> p (db.find<package> (n));
    t.commit ();

    if (p == nullptr)
      fail << "package " << n << " does not exist in configuration " << c;

    if (p->state != state::configured)
      fail << "package " << n << " is " << p->state <<
        info << "expected it to be configured";

    level4 ([&]{trace << p->name << " " << p->version;});

    assert (p->out_root); // Should be present since configured.
    dir_path out_root (c / *p->out_root); // Always relative.
    level4 ([&]{trace << "out_root: " << out_root;});

    // Form the buildspec.
    //
    string bspec (cmd + "(" + out_root.string () + "/)");
    level4 ([&]{trace << "buildspec: " << bspec;});

    run_b (bspec);

    if (verb)
      text << cmd << (cmd.back () != 'e' ? "ed " : "d ")
           << p->name << " " << p->version;
  }
}
