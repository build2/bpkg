// file      : bpkg/pkg-status.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_STATUS_HXX
#define BPKG_PKG_STATUS_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/pkg-status-options.hxx>

namespace bpkg
{
  int
  pkg_status (const pkg_status_options&, cli::scanner& args);
}

#endif // BPKG_PKG_STATUS_HXX
