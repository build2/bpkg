// file      : bpkg/cfg-unlink.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_CFG_UNLINK_HXX
#define BPKG_CFG_UNLINK_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/cfg-unlink-options.hxx>

namespace bpkg
{
  int
  cfg_unlink (const cfg_unlink_options&, cli::scanner& args);
}

#endif // BPKG_CFG_UNLINK_HXX
