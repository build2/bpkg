// file      : bpkg/rep-info.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_REP_INFO_HXX
#define BPKG_REP_INFO_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/rep-info-options.hxx>

namespace bpkg
{
  int
  rep_info (const rep_info_options&, cli::scanner& args);
}

#endif // BPKG_REP_INFO_HXX
