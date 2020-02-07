// file      : bpkg/help.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_HELP_HXX
#define BPKG_HELP_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/help-options.hxx>

namespace bpkg
{
  using usage_function = cli::usage_para (ostream&, cli::usage_para);

  int
  help (const help_options&, const string& topic, usage_function* usage);
}

#endif // BPKG_HELP_HXX
