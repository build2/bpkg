// file      : bpkg/system-package-manager.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SYSTEM_PACKAGE_MANAGER_HXX
#define BPKG_SYSTEM_PACKAGE_MANAGER_HXX

#include <map>

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
  class system_package_status
  {
  public:
    // Downstream (as in, bpkg package) version.
    //
    bpkg::version version;

    // The system package can be either "available already installed",
    // "available partially installed" (for example, libfoo but not
    // libfoo-dev is installed) or "available not yet installed".
    //
    // Whether not_installed versions can be returned along with installed
    // or partially_installed depends on whether the packager manager can
    // install multiple versions side-by-side.
    //
    enum status_type {installed, partially_installed, not_installed};

    status_type status = not_installed;

    // System (as in, distribution package) name and version.
    //
    // @@ But these could be multiple. Do we really need this?
    // @@ Can now probably provide as virtual functions.
    /*
    string system_name;
    string system_version;
    */

  public:
    virtual
    ~system_package_status ();

    system_package_status () = default;
  };

  class system_package_manager
  {
  public:
    // Query the system package status.
    //
    // This function has two modes: cache-only (available_packages is NULL)
    // and full (available_packages is not NULL). In the cache-only mode this
    // function returns the status of this package if it has already been
    // queried and NULL otherwise. This allows the caller to only collect all
    // the available packages (for the name/version mapping information) if
    // really necessary.
    //
    // The returned list should be arranged in the preference order with the
    // first entry having the highest preference. Normally this will be in the
    // descending version order but can also be something more elaborate, such
    // as the already installed or partially installed version coming first
    // with the descending version order after that.
    //
    // The returned list can be empty, which indicates that no such package
    // is available from the system package manager. Note that empty is also
    // returned if no fully installed package is available from the system and
    // the install argument is false.
    //
    // If fetch is false, then do not re-fetch the system package repository
    // metadata (that is, available packages/versions) before querying for
    // the available version of the not yet installed or partially installed
    // packages.
    //
    virtual const vector<unique_ptr<system_package_status>>*
    pkg_status (const package_name&,
                const available_packages*,
                bool install,
                bool fetch) = 0;

    // Install the previously-queried package that is not installed or
    // partially installed.
    //
    // Return false if the installation was aborted by the user (for example,
    // the user answered 'N' to the prompt). @@ Do we really need this? We
    // may not always be able to distinguish.
    //
    virtual bool
    pkg_install (const package_name&, const version&) = 0;

  public:
    virtual
    ~system_package_manager ();

    explicit
    system_package_manager (os_release&& osr)
        : os_release_ (osr) {}

  protected:
    // Given the available packages (as returned by find_available_all())
    // return the list of system package names as mapped by the
    // <distribution>-name values.
    //
    // The name_id, version_id, and like_ids are the values from os_release
    // (refer there for background). If version_id is empty, then it's treated
    // as "0".
    //
    // First consider <distribution>-name values corresponding to name_id.
    // Assume <distribution> has the <name>[_<version>] form, where <version>
    // is a semver-like version (e.g, 10, 10.15, or 10.15.1) and return all
    // the values that are equal of less than the specified version_id
    // (include the value with the absent <version>). In a sense, absent
    // <version> can be treated as 0 semver-like versions.
    //
    // If no value is found then repeat the above process for every like_ids
    // entry (from left to right) instead of name_id with version_id equal 0.
    //
    // If still no value is found, then return empty list (in which case the
    // caller may choose to fallback to the downstream package name or do
    // something more elaborate, like translate version_id to the like_id's
    // version and try that).
    //
    // @@ TODO: allow multiple -name values per same distribution and handle
    //    here? E.g., libcurl4-openssl-dev libcurl4-gnutls-dev. But they will
    //    have the same available version, how will we deal with that? How
    //    will we pick one? Perhaps this should all be handled by the system
    //    package manager (conceptually, this is configuration negotiation).
    //
    static strings
    system_package_names (const available_packages&,
                          const string& name_id,
                          const string& version_id,
                          const vector<string>& like_ids);

    // Given the system package version and available packages (as returned by
    // find_available_all()) return the downstream package version as mapped
    // by one of the <distribution>-to-downstream-version values.
    //
    // The rest of the arguments as well as the overalls semantics is the same
    // as in system_package_names() above. That is, first consider
    // <distribution>-to-downstream-version values corresponding to
    // name_id. If none match, then repeat the above process for every
    // like_ids entry with version_id equal 0. If still no match, then return
    // nullopt.
    //
    static optional<version>
    downstream_package_version (const string& system_version,
                                const available_packages&,
                                const string& name_id,
                                const string& version_id,
                                const vector<string>& like_ids);
  protected:
    os_release os_release_;
    std::map<package_name,
             vector<unique_ptr<system_package_status>>> status_cache_;
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
