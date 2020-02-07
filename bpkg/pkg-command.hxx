// file      : bpkg/pkg-command.hxx -*- C++ -*-
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
  // If cmd_variant is not empty, then the <cmd>-for-<variant> is performed
  // instead.
  //
  // The command can also be performed recursively for all or immediate
  // dependencies of the specified or all the held packages.
  //
  int
  pkg_command (const string& cmd, // Without the 'pkg-' prefix.
               const configuration_options&,
               const string& cmd_variant,
               bool recursive,
               bool immediate,
               bool all,
               bool package_cwd,
               cli::group_scanner& args);

  struct pkg_command_vars
  {
    shared_ptr<selected_package> pkg;
    strings                     vars; // Package-specific command line vars.

    bool cwd; // Change the working directory to the package directory.
  };

  void
  pkg_command (const string& cmd,
               const dir_path& configuration,
               const common_options&,
               const string& cmd_variant,
               const strings& common_vars,
               const vector<pkg_command_vars>&);
}

#endif // BPKG_PKG_COMMAND_HXX
