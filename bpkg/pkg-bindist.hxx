// file      : bpkg/pkg-bindist.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_BINDIST_HXX
#define BPKG_PKG_BINDIST_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/pkg-command.hxx>
#include <bpkg/pkg-bindist-options.hxx>

namespace bpkg
{
  // Note that for now it doesn't seem we need to bother with package-
  // specific configuration variables so it's scanner instead of
  // group_scanner.
  //
  int
  pkg_bindist (const pkg_bindist_options&, cli::scanner&);

  pkg_bindist_options
  merge_options (const default_options<pkg_bindist_options>&,
                 const pkg_bindist_options&);
}

#endif // BPKG_PKG_BINDIST_HXX
