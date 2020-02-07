// file      : bpkg/cfg-create.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_CFG_CREATE_HXX
#define BPKG_CFG_CREATE_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/cfg-create-options.hxx>

namespace bpkg
{
  int
  cfg_create (const cfg_create_options&, cli::scanner& args);

  default_options_files
  options_files (const char* cmd,
                 const cfg_create_options&,
                 const strings& args);

  cfg_create_options
  merge_options (const default_options<cfg_create_options>&,
                 const cfg_create_options&);
}

#endif // BPKG_CFG_CREATE_HXX
