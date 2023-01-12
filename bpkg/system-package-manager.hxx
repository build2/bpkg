// file      : bpkg/system-package-manager.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SYSTEM_PACKAGE_MANAGER_HXX
#define BPKG_SYSTEM_PACKAGE_MANAGER_HXX

//#include <libbpkg/manifest.hxx>     // version
//#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

namespace bpkg
{
  // The system package manager interface. Used by both pkg-build (to query
  // and install system packages) and by pkg-bindist (to build them).
  //
  class system_package_manager
  {
  public:
    virtual
    ~system_package_manager ();
  };

  // Create a package manager instance corresponding to the specified host
  // target and optional manager type. If type is empty, return NULL if there
  // is no support for this platform.
  //
  unique_ptr<system_package_manager>
  make_system_package_manager (const target_triplet&,
                               const string& type);
}

#endif // BPKG_SYSTEM_PACKAGE_MANAGER_HXX
