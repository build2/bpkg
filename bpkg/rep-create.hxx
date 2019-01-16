// file      : bpkg/rep-create.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
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
}

#endif // BPKG_REP_CREATE_HXX
