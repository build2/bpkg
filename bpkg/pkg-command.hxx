// file      : bpkg/pkg-command.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_COMMAND_HXX
#define BPKG_PKG_COMMAND_HXX

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // selected_package
#include <bpkg/utility.hxx>

#include <bpkg/configuration-options.hxx>

namespace bpkg
{
  // Common pkg-{update,clean,test,install,...} implementation.
  //
  int
  pkg_command (const string& cmd, // Without the 'pkg-' prefix.
               const configuration_options&,
               cli::scanner& args);

  struct pkg_command_vars
  {
    shared_ptr<selected_package> pkg;
    strings vars;
  };

  void
  pkg_command (const string& cmd,
               const dir_path& configuration,
               const common_options&,
               const strings& common_vars,
               const vector<pkg_command_vars>&);
}

#endif // BPKG_PKG_COMMAND_HXX