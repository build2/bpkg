// file      : bpkg/cfg-info.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_CFG_INFO_HXX
#define BPKG_CFG_INFO_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/cfg-info-options.hxx>

namespace bpkg
{
  int
  cfg_info (const cfg_info_options&, cli::scanner& args);
}

#endif // BPKG_CFG_INFO_HXX
