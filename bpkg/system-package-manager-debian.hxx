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
    virtual optional<const package_status*>
    pkg_status (const package_name&,
                const available_packages*,
                bool install,
                bool fetch) override;

  public:
    explicit
    system_package_manager_debian (os_release&& osr)
        : system_package_manager (move (osr)) {}

  protected:
  };
}

#endif // BPKG_SYSTEM_PACKAGE_MANAGER_DEBIAN_HXX
