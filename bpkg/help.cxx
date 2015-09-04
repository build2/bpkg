// file      : bpkg/help.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/help>

#include <cassert>
#include <iostream>

#include <bpkg/types>
#include <bpkg/diagnostics>

#include <bpkg/bpkg-options>

using namespace std;

namespace bpkg
{
  static void
  help ()
  {
    ostream& o (cout);

    o << "usage: bpkg --help" << endl
      << "       bpkg --version" << endl
      << "       bpkg [<common-options>] <command> [<command-options>] " <<
      "[<command-args>]" << endl
      << endl;

    o << "The commands are:" << endl
      << endl;

    bpkg_commands::print_short_usage (o);
    o << endl;

    o << "The help topics are:" << endl
      << endl;

    bpkg_topics::print_short_usage (o);
    o << endl;

    o << "The common options are:" << endl
      << endl;

    common_options::print_short_usage (o);
    o << endl;

    o << "The common options can also be specified as part of the command-" <<
      "specific ones."<< endl;
  }

  void
  help (const help_options&, const string& t, void (*usage) (std::ostream&))
  {
    if (usage != nullptr)    // Command.
      usage (cout);
    else if (t.empty ())     // General help.
      help ();
    else if (t == "options") // Help topics.
    {
      common_options::print_long_usage (cout);
    }
    else
      fail << "unknown bpkg command/help topic '" << t << "'" <<
        info << "run 'bpkg help' for more information";
  }
}
