// file      : bpkg/pkg-uninstall.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_UNINSTALL_HXX
#define BPKG_PKG_UNINSTALL_HXX

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // selected_package
#include <bpkg/utility.hxx>

#include <bpkg/pkg-command.hxx>
#include <bpkg/pkg-uninstall-options.hxx>

namespace bpkg
{
  inline int
  pkg_uninstall (const pkg_uninstall_options& o, cli::group_scanner& args)
  {
    return pkg_command ("uninstall", o,
                        "" /* cmd_variant */,
                        o.recursive (),
                        o.immediate (),
                        o.all (),
                        false /* package_cwd */,
                        args);
  }
}

#endif // BPKG_PKG_UNINSTALL_HXX
