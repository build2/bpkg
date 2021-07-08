// file      : bpkg/cfg-create.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_CFG_CREATE_HXX
#define BPKG_CFG_CREATE_HXX

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // configuration
#include <bpkg/utility.hxx>

#include <bpkg/cfg-create-options.hxx>

namespace bpkg
{
  int
  cfg_create (const cfg_create_options&, cli::scanner& args);

  // Create a new bpkg configuration, initialize its database (add self-link,
  // root repository, etc), and return this configuration information. See
  // bpkg-cfg-create(1) for arguments semantics.
  //
  // If there is a current transaction already open, then stash it before the
  // database initialization and restore it afterwards (used to create private
  // configuration on demand).
  //
  shared_ptr<configuration>
  cfg_create (const common_options&,
              const dir_path&,
              optional<string> name,
              string type,
              const strings& mods,
              const strings& vars,
              bool existing,
              bool wipe,
              optional<uuid> uid = nullopt,
              const optional<dir_path>& host_config = nullopt,
              const optional<dir_path>& build2_config = nullopt);

  default_options_files
  options_files (const char* cmd,
                 const cfg_create_options&,
                 const strings& args);

  cfg_create_options
  merge_options (const default_options<cfg_create_options>&,
                 const cfg_create_options&);
}

#endif // BPKG_CFG_CREATE_HXX
