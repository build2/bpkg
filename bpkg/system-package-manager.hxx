// file      : bpkg/system-package-manager.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SYSTEM_PACKAGE_MANAGER_HXX
#define BPKG_SYSTEM_PACKAGE_MANAGER_HXX

#include <libbpkg/manifest.hxx>     // version
#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>
#include <bpkg/host-os-release.hxx>

namespace bpkg
{
  // The system package manager interface. Used by both pkg-build (to query
  // and install system packages) and by pkg-bindist (to build them).
  //
  class system_package_manager
  {
  public:
    struct package_status
    {
      bpkg::version version;

      // The system package can be either "available already installed"
      // or "available not yet installed".
      //
      // If the package is partially installed (for example, libfoo but not
      // libfoo-dev is installed), then installed should be false (and perhaps
      // only a single available version should be returned).
      //
      bool installed;

      string system_name;
      string system_version;
    };

    // @@ We actually need to fetch is some are not installed to get their
    //    versions. We can do it as part of the call, no?

  public:
    virtual
    ~system_package_manager ();

    explicit
    system_package_manager (os_release&& osr)
        : os_release_ (osr) {}

  protected:
    // Given the available packages (as returned by find_available_all())
    // return the list of system package names.
    //
    // The name_id, version_id, and like_id are the values from the os_release
    // struct (refer there for background). If version_id is empty, then it's
    // treated as "0".
    //
    // First consider <distribution>-name values corresponding to name_id.
    // Assume <distribution> has the <name>[_<version>] form, where <version>
    // is a semver-like version (e.g, 10, 10.15, or 10.15.1) and return all
    // the values that are equal of less than the specified version_id
    // (include the value with the absent <version>). In a sense, absent
    // <version> can be treated as 0 semver-like versions.
    //
    // If no value is found and like_id is not empty, then repeat the above
    // process for like_id instead of name_id and version_id equal 0.
    //
    // If still no value is found, then return empty list (in which case the
    // caller may choose to fallback to the downstream package name or do
    // something more elaborate, like translate version_id to the like_id's
    // version and try that).
    //
    static strings
    system_package_names (const available_packages&,
                          const string& name_id,
                          const string& version_id,
                          const string& like_id);

  protected:
    os_release os_release_;
  };

  // Create a package manager instance corresponding to the specified host
  // target and optional manager name. If name is empty, return NULL if there
  // is no support for this platform.
  //
  // @@ TODO: need to assign names. Ideas:
  //
  //    dpkg-apt, rpm-dnf
  //    deb, rpm
  //    debian, fedora (i.e., follow  /etc/os-release ID_LIKE lead)
  //
  unique_ptr<system_package_manager>
  make_system_package_manager (const target_triplet&,
                               const string& name);
}

#endif // BPKG_SYSTEM_PACKAGE_MANAGER_HXX
