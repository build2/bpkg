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
  // If allow_host_type is false, then fail if the current configuration is of
  // the host or build2 type. Also skip the build-time dependencies in the
  // recursive mode in this case.
  //
  // Note: loads selected packages.
  //
  int
  pkg_command (const string& cmd, // Without the 'pkg-' prefix.
               const configuration_options&,
               const string& cmd_variant,
               bool recursive,
               bool immediate,
               bool all,
               const strings& all_patterns,
               bool package_cwd,
               bool allow_host_type,
               cli::group_scanner& args);

  struct pkg_command_vars
  {
    // Configuration information.
    //
    // Used to derive the package out_root directory, issue diagnostics, etc.
    //
    // Note that we cannot store the database reference here since it can be
    // closed by the time this information is used. Instead, we save the
    // required information.
    //
    dir_path                     config_orig; // Database's config_orig.
    bool                         config_main; // True if database is main.

    shared_ptr<selected_package> pkg;
    strings                      vars; // Package-specific command line vars.

    bool cwd; // Change the working directory to the package directory.

    // Return the selected package name/version followed by the configuration
    // directory, unless this is the current configuration. For example:
    //
    // libfoo/1.1.0
    // libfoo/1.1.0 [cfg/]
    //
    std::string
    string () const;
  };

  void
  pkg_command (const string& cmd,
               const common_options&,
               const string& cmd_variant,
               const strings& common_vars,
               const vector<pkg_command_vars>&);
}

#endif // BPKG_PKG_COMMAND_HXX
