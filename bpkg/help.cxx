// file      : bpkg/help.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/help.hxx>

#include <libbutl/pager.hxx>

#include <bpkg/diagnostics.hxx>
#include <bpkg/bpkg-options.hxx>

// Help topics.
//
#include <bpkg/repository-signing.hxx>
#include <bpkg/repository-types.hxx>
#include <bpkg/argument-grouping.hxx>
#include <bpkg/default-options-files.hxx>

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
      else if (t == "repository-types")
        usage = &print_bpkg_repository_types_usage;
      else if (t == "argument-grouping")
        usage = &print_bpkg_argument_grouping_usage;
      else if (t == "default-options-files")
        usage = &print_bpkg_default_options_files_usage;
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
