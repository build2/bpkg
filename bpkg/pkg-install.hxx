// file      : bpkg/pkg-install.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_INSTALL_HXX
#define BPKG_PKG_INSTALL_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/pkg-command.hxx>
#include <bpkg/pkg-install-options.hxx>

namespace bpkg
{
  // Note that we disallow installing packages from the host/build2
  // configurations. The reason for that is that otherwise we can fail, trying
  // to build a package both for install and normally (as a dependency).
  //
  inline int
  pkg_install (const pkg_install_options& o, cli::group_scanner& args)
  {
    return pkg_command ("install",
                        o,
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

#endif // BPKG_PKG_INSTALL_HXX
