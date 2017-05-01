// file      : bpkg/rep-add.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_REP_ADD_HXX
#define BPKG_REP_ADD_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/rep-add-options.hxx>

namespace bpkg
{
  int
  rep_add (const rep_add_options&, cli::scanner& args);
}

#endif // BPKG_REP_ADD_HXX
