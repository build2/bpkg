// file      : bpkg/pkg-update.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_UPDATE_HXX
#define BPKG_PKG_UPDATE_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/pkg-command.hxx>
#include <bpkg/pkg-update-options.hxx>

namespace bpkg
{
  inline int
  pkg_update (const pkg_update_options& o, cli::group_scanner& args)
  {
    return pkg_command ("update",
                        o,
                        o.for_ (),
                        o.recursive (),
                        o.immediate (),
                        o.all (),
                        o.all_pattern (),
                        false /* package_cwd */,
                        true /* allow_host_type */,
                        args);
  }

  inline void
  pkg_update (const common_options& o,
              const string& cmd_variant,
              const strings& common_vars,
              const vector<pkg_command_vars>& pkgs)
  {
    pkg_command ("update", o, cmd_variant, common_vars, pkgs);
  }
}

#endif // BPKG_PKG_UPDATE_HXX
