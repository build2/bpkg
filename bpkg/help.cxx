// file      : bpkg/help.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/help>

#include <iostream>

#include <bpkg/types>
#include <bpkg/utility>
#include <bpkg/diagnostics>

#include <bpkg/bpkg-options>

using namespace std;

namespace bpkg
{
  static void
  help ()
  {
    ostream& o (cout);

    //@@ TODO

    o << "usage: bpkg --help" << endl
      << "       bpkg --version" << endl
      << "       bpkg help [<command>|<topic>]" << endl
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

    o << "The common options are summarized below. Note that they can also "
      "be specified" << endl
      << "as part of the command-specific options." << endl
      << endl;

    common_options::print_short_usage (o);
    o << endl;

    o << ""<< endl;
  }

  int
  help (const help_options&, const string& t, void (*usage) (std::ostream&))
  {
    ostream& o (cout);

    if (usage != nullptr)    // Command.
    {
      usage (o);

      //@@ TODO

      o << endl
        << "The common options are summarized below. For details, see the " <<
        "'common' help" << endl
        << "topic." << endl
        << endl;

      common_options::print_short_usage (o);
    }
    else if (t.empty ())     // General help.
      help ();
    else if (t == "common")  // Help topics.
    {
      print_bpkg_common_long_usage (cout);
    }
    else
      fail << "unknown bpkg command/help topic '" << t << "'" <<
        info << "run 'bpkg help' for more information";

    return 0;
  }
}
