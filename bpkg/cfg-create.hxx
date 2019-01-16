// file      : bpkg/cfg-create.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
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
}

#endif // BPKG_CFG_CREATE_HXX
