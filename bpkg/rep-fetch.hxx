// file      : bpkg/rep-fetch.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_REP_FETCH_HXX
#define BPKG_REP_FETCH_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/rep-fetch-options.hxx>

namespace bpkg
{
  int
  rep_fetch (const rep_fetch_options&, cli::scanner& args);
}

#endif // BPKG_REP_FETCH_HXX
