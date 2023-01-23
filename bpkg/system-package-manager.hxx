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
  // The system/distribution package manager interface. Used by both pkg-build
  // (to query and install system packages) and by pkg-bindist (to build
  // them).
  //
  // Note that currently the result of a query is a single available version.
  // While some package managers may support having multiple available
  // versions and may even allow installing multiple versions in parallel,
  // supporting this on our side will complicate things quite a bit. While we
  // can probably plug multiple available versions into our constraint
  // satisfaction machinery, the rabbit hole goes deeper than that since, for
  // example, different bpkg packages can be mapped to the same system
  // package, as is the case for libcrypto/libssl which are both mapped to
  // libssl on Debian. This means we will need to somehow coordinate (and
  // likely backtrack) version selection between unrelated bpkg packages
  // because only one underlying system version can be selected. (One
  // simplified way to handle this would be to detect that different versions
  // we selected and fail asking the user to resolve this manually.)
  //
  // Additionally, parallel installation is unlikely to be suppored for the
  // packages we are interested in due to the underlying limitations.
  // Specifically, the packages that we are primarily interested in are
  // libraries with headers and executables (tools). While most package
  // managers (e.g., Debian, Fedora) are able to install multiple libraries in
  // parallel, they normally can only install a single set of headers, static
  // libraries, pkg-config files, etc., (e.g., -dev/-devel package) at a time
  // due to them being installed into the same location (e.g.,
  // /usr/include). The same holds for executables, which are installed into
  // the same location (e.g., /usr/bin).
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
  // languages (e.g., Rust) can always co-exist. Plus, even in the case of
  // C/C++ libraries, there is still the plausible case of picking one of
  // the multiple available version.
  //
  // On the other hand, the ultimate goal of system package managers, at least
  // traditional ones like Debian and Fedora, is to end up with a single,
  // usually the latest available, version of the package that is used by
  // everyone. In fact, if one looks at a stable distributions of Debian and
  // Fedora, they normally provide only a single version of each package. This
  // decision will also likely simplify the implementation. For example, on
  // Debian, it's straightforward to get the installed and candidate versions
  // (e.g., from apt-cache policy). But getting all the possible versions that
  // can be installed without having to specify the release explicitly is a
  // lot less straightforward (see the apt-cache command documentation in The
  // Debian Administrator's Handbook for background).
  //
  // So for now we keep it simple and pick a single available version but can
  // probably revise this decision later.
  //
  class system_package_status
  {
  public:
    // Downstream (as in, bpkg package) version.
    //
    bpkg::version version;

    // System (as in, distribution package) package name and version for
    // diagnostics.
    //
    // Note that this status may represent multiple system packages (for
    // example, libssl3 and libssl3-dev) and here we have the main package
    // name (for example, libssl3).
    //
    string system_name;
    string system_version;

    // The system package can be either "available already installed",
    // "available partially installed" (for example, libfoo but not
    // libfoo-dev is installed) or "available not yet installed".
    //
    enum status_type {installed, partially_installed, not_installed};

    status_type status = not_installed;

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
    // queried and nullopt otherwise. This allows the caller to only collect
    // all the available packages (for the name/version mapping information)
    // if really necessary.
    //
    // The returned status can be NULL, which indicates that no such package
    // is available from the system package manager. Note that empty is also
    // returned if no fully installed package is available from the system and
    // the install argument is false.
    //
    // If fetch is false, then do not re-fetch the system package repository
    // metadata (that is, available packages/versions) before querying for the
    // available version of the not yet installed or partially installed
    // packages.
    //
    virtual optional<const system_package_status*>
    pkg_status (const package_name&,
                const available_packages*,
                bool install,
                bool fetch) = 0;

    // Install the specified subset of the previously-queried packages.
    //
    // Note that this function should be called only once after the final set
    // of the necessary system packages has been determined. And the specified
    // subset should contain all the selected packages, including the already
    // fully installed. This allows the implementation to merge and de-
    // duplicate the system package set to be installed (since some bpkg
    // packages may be mapped to the same system package), perform post-
    // installation verifications (such as making sure the versions of already
    // installed packages have not changed due to upgrades), implement version
    // holds, etc.
    //
    // Note also that the implementation is expected to issue appropriate
    // progress and diagnostics.
    //
    virtual void
    pkg_install (const vector<package_name>&, bool install) = 0;

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
    // nullopt (in which case the caller may choose to fallback to the system
    // package version or do something more elaborate, like translate
    // version_id to the like_id's version and try that).
    //
    static optional<version>
    downstream_package_version (const string& system_version,
                                const available_packages&,
                                const string& name_id,
                                const string& version_id,
                                const vector<string>& like_ids);
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
