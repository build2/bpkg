// file      : bpkg/system-package-manager-debian.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SYSTEM_PACKAGE_MANAGER_DEBIAN_HXX
#define BPKG_SYSTEM_PACKAGE_MANAGER_DEBIAN_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/system-package-manager.hxx>

namespace bpkg
{
  // The system package manager implementation for Debian and alike (Ubuntu,
  // etc) using the APT frontend.
  //
  class system_package_manager_debian: public system_package_manager
  {
  public:
    virtual const vector<unique_ptr<system_package_status>>*
    pkg_status (const package_name&,
                const available_packages*,
                bool install,
                bool fetch) override;

    virtual bool
    pkg_install (const package_name&, const version&) override;

  public:
    // Note: expects os_release::name_id to be "debian" or os_release::like_id
    // to contain "debian".
    //
    explicit
    system_package_manager_debian (os_release&& osr)
        : system_package_manager (move (osr)) {}

  protected:
    bool fetched_ = false; // True if already fetched metadata.
  };
}

#endif // BPKG_SYSTEM_PACKAGE_MANAGER_DEBIAN_HXX
