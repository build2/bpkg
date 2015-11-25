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
    print_bpkg_usage (cout);
  }

  int
  help (const help_options&, const string& t, usage_function* usage)
  {
    ostream& o (cout);

    if (usage != nullptr)    // Command.
      usage (o, cli::usage_para::none);
    else if (t.empty ())             // General help.
      help ();
    else if (t == "common-options")  // Help topics.
    {
      print_bpkg_common_options_long_usage (cout);
    }
    else
      fail << "unknown bpkg command/help topic '" << t << "'" <<
        info << "run 'bpkg help' for more information";

    return 0;
  }
}
