// file      : bpkg/pkg-build.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_BUILD_HXX
#define BPKG_PKG_BUILD_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/pkg-build-options.hxx>

namespace bpkg
{
  int
  pkg_build (const pkg_build_options&, cli::group_scanner& args);
}

#endif // BPKG_PKG_BUILD_HXX
