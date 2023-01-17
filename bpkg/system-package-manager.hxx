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
      // Downstream (as in, bpkg package) version.
      //
      bpkg::version version;

      // The system package can be either "available already installed",
      // "available partially installed" (for example, libfoo but not
      // libfoo-dev is installed) or "available not yet installed".
      //
      enum {installed, partially_installed, not_installed} status;

      // System (as in, distribution package) name and version.
      //
      // @@ But these could be multiple. Do we really need this?
      /*
      string system_name;
      string system_version;
      */
    };

    // Query the system package status.
    //
    // This function has two modes: cache-only (available_packages is NULL)
    // and full (available_packages is not NULL). In the cache-only mode this
    // function returns the status of this package if it has already been
    // queried and nullopt otherwise. This allows the caller to only collect
    // all the available packages (for the name/version mapping information)
    // if really necessary.
    //
    // The returned value can be NULL, which indicates that no such package is
    // available from the system package manager. Note that NULL is returned
    // if no fully installed package is available from the system and install
    // is false.
    //
    // If fetch is false, then do not re-fetch the system package repository
    // metadata (that is, available packages/versions) before querying for
    // the available version of the not yet installed or partially installed
    // packages.
    //
    // Note that currently the result is a single available version. While
    // some package managers may support installing multiple versions in
    // parallel, this is unlikely to be suppored for the packages we are
    // interested in due to the underlying limitations.
    //
    // Specifically, the packages that we are primarily interested in are
    // libraries with headers and executables (tools). While most package
    // managers (e.g., Debian, Fedora) are able to install multiple libraries
    // in parallel, they normally can only install a single set of headers,
    // static libraries, pkg-config files, etc., (e.g., -dev/-devel package)
    // at a time due to them being installed into the same location (e.g.,
    // /usr/include). The same holds for executables, which are installed into
    // the same location (e.g., /usr/bin).
    //
    // @@ But it's still plausible to have multiple available versions but
    //    only being able to install one at a time?
    //
    // It is possible that a certain library has made arrangements for
    // multiple of its versions to co-exist. For example, hypothetically, our
    // libssl package could be mapped to both libssl1.1 libssl1.1-dev and
    // libssl3 libssl3-dev which could be installed at the same time (note
    // that it is not the case in reality; there is only libssl-dev). However,
    // in this case, we should probably also have two packages with separate
    // names (e.g., libssl and libssl3) that can also co-exist. An example of
    // this would be libQt5Core and libQt6Core. (Note that strictly speaking
    // there could be different degrees of co-existence: for the system
    // package manager it is sufficient for different versions not to clobber
    // each other's files while for us we may also need the ability to use
    // different versions in the base build).
    //
    // Note also that the above reasoning is quite C/C++-centric and it's
    // possible that multiple versions of libraries (or equivalent) for other
    // languages (e.g., Rust) can always co-exist. In this case we may need to
    // revise this decision to only return a single version (and pick the best
    // suitable version as part of the constraint resolution).
    //
    virtual optional<const package_status*>
    pkg_status (const package_name&,
                const available_packages*,
                bool install,
                bool fetch) = 0;

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
