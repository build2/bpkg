// file      : bpkg/pkg-clean.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_CLEAN_HXX
#define BPKG_PKG_CLEAN_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/pkg-command.hxx>
#include <bpkg/pkg-clean-options.hxx>

namespace bpkg
{
  inline int
  pkg_clean (const pkg_clean_options& o, cli::group_scanner& args)
  {
    return pkg_command ("clean",
                        o,
                        "" /* cmd_variant */,
                        false /* recursive */,
                        false /* immediate */,
                        o.all (),
                        false /* package_cwd */,
                        args);
  }
}

#endif // BPKG_PKG_CLEAN_HXX
