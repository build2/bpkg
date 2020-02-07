// file      : bpkg/rep-info.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_REP_INFO_HXX
#define BPKG_REP_INFO_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/rep-info-options.hxx>

namespace bpkg
{
  int
  rep_info (const rep_info_options&, cli::scanner& args);

  default_options_files
  options_files (const char* cmd,
                 const rep_info_options&,
                 const strings& args);

  rep_info_options
  merge_options (const default_options<rep_info_options>&,
                 const rep_info_options&);
}

#endif // BPKG_REP_INFO_HXX
