// file      : bpkg/rep-create.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_REP_CREATE_HXX
#define BPKG_REP_CREATE_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/rep-create-options.hxx>

namespace bpkg
{
  int
  rep_create (const rep_create_options&, cli::scanner& args);

  default_options_files
  options_files (const char* cmd,
                 const rep_create_options&,
                 const strings& args);

  rep_create_options
  merge_options (const default_options<rep_create_options>&,
                 const rep_create_options&);
}

#endif // BPKG_REP_CREATE_HXX
