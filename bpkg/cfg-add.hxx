// file      : bpkg/cfg-add.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_CFG_ADD_HXX
#define BPKG_CFG_ADD_HXX

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // configuration
#include <bpkg/utility.hxx>

#include <bpkg/cfg-add-options.hxx>

namespace bpkg
{
  int
  cfg_add (const cfg_add_options&, cli::scanner& args);

  // Associate the configuration specified as the directory path with the
  // current configuration, attach the associated configuration database, and
  // return the association. Note that it also establishes an implicit
  // association of the current configuration with the associated one.
  //
  // The specified configuration path must be absolute and normalized. If the
  // relative argument is true, then rebase this path relative to the current
  // configuration directory path and fail if that's not possible (different
  // drive on Windows, etc).
  //
  // If the current configuration database has its explicit associations
  // pre-attached, then also pre-attach explicit associations of the newly
  // associated database.
  //
  shared_ptr<configuration>
  cfg_add (database&,
           const dir_path&,
           bool relative,
           optional<string> name,
           bool sys_rep = false);
}

#endif // BPKG_CFG_ADD_HXX
