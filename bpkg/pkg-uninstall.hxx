// file      : bpkg/pkg-uninstall.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_UNINSTALL_HXX
#define BPKG_PKG_UNINSTALL_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/pkg-command.hxx>
#include <bpkg/pkg-uninstall-options.hxx>

namespace bpkg
{
  // Note that we disallow uninstalling packages from the host/build2
  // configurations (see pkg_install() for details).
  //
  inline int
  pkg_uninstall (const pkg_uninstall_options& o, cli::group_scanner& args)
  {
    return pkg_command ("uninstall", o,
                        "" /* cmd_variant */,
                        o.recursive (),
                        o.immediate (),
                        o.all (),
                        o.all_pattern (),
                        false /* package_cwd */,
                        false /* allow_host_type */,
                        args);
  }
}

#endif // BPKG_PKG_UNINSTALL_HXX
