// file      : bpkg/cfg-add.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_CFG_ADD_HXX
#define BPKG_CFG_ADD_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/cfg-add-options.hxx>

namespace bpkg
{
  int
  cfg_add (const cfg_add_options&, cli::scanner& args);
}

#endif // BPKG_CFG_ADD_HXX
