// file      : bpkg/rep-remove.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_REP_REMOVE_HXX
#define BPKG_REP_REMOVE_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/rep-remove-options.hxx>

namespace bpkg
{
  int
  rep_remove (const rep_remove_options&, cli::scanner& args);
}

#endif // BPKG_REP_REMOVE_HXX
