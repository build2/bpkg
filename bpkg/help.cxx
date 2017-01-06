// file      : bpkg/help.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/help>

#include <butl/pager>

#include <bpkg/diagnostics>
#include <bpkg/bpkg-options>

// Help topics.
//
#include <bpkg/repository-signing>

using namespace std;
using namespace butl;

namespace bpkg
{
  int
  help (const help_options& o, const string& t, usage_function* usage)
  {
    if (usage == nullptr) // Not a command.
    {
      if (t.empty ())             // General help.
        usage = &print_bpkg_usage;
      //
      // Help topics.
      //
      else if (t == "common-options")
        usage = &print_bpkg_common_options_long_usage;
      else if (t == "repository-signing")
        usage = &print_bpkg_repository_signing_usage;
      else
        fail << "unknown bpkg command/help topic '" << t << "'" <<
          info << "run 'bpkg help' for more information";
    }

    try
    {
      pager p ("bpkg " + (t.empty () ? "help" : t),
               verb >= 2,
               o.pager_specified () ? &o.pager () : nullptr,
               &o.pager_option ());

      usage (p.stream (), cli::usage_para::none);

      // If the pager failed, assume it has issued some diagnostics.
      //
      return p.wait () ? 0 : 1;
    }
    // Catch io_error as std::system_error together with the pager-specific
    // exceptions.
    //
    catch (const system_error& e)
    {
      error << "pager failed: " << e;

      // Fall through.
    }

    throw failed ();
  }
}
