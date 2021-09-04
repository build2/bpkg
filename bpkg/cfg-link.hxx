// file      : bpkg/cfg-link.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_CFG_LINK_HXX
#define BPKG_CFG_LINK_HXX

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // configuration
#include <bpkg/utility.hxx>

#include <bpkg/cfg-link-options.hxx>

namespace bpkg
{
  int
  cfg_link (const cfg_link_options&, cli::scanner& args);

  // Link the configuration specified as the directory path with the current
  // configuration, attach the linked configuration database, and return the
  // link. Note that it also establishes an implicit backlink of the current
  // configuration with the linked one.
  //
  // The specified configuration path must be absolute and normalized. If the
  // relative argument is true, then rebase this path relative to the current
  // configuration directory path and fail if that's not possible (different
  // drive on Windows, etc).
  //
  // If the current configuration database has its explicit links pre-
  // attached, then also pre-attach explicit links of the newly linked
  // database.
  //
  shared_ptr<configuration>
  cfg_link (database&,
            const dir_path&,
            bool relative,
            optional<string> name,
            bool sys_rep = false);
}

#endif // BPKG_CFG_LINK_HXX
