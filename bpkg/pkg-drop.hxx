// file      : bpkg/pkg-drop.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_DROP_HXX
#define BPKG_PKG_DROP_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/pkg-drop-options.hxx>

namespace bpkg
{
  int
  pkg_drop (const pkg_drop_options&, cli::scanner& args);
}

#endif // BPKG_PKG_DROP_HXX
