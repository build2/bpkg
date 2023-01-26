// file      : bpkg/system-package-manager-debian.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SYSTEM_PACKAGE_MANAGER_DEBIAN_HXX
#define BPKG_SYSTEM_PACKAGE_MANAGER_DEBIAN_HXX

#include <map>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/system-package-manager.hxx>

namespace bpkg
{
  // The system package manager implementation for Debian and alike (Ubuntu,
  // etc) using the APT frontend.
  //
  // For background, a library in Debian is normally split up into several
  // packages: the shared library package (e.g., libfoo1 where 1 is the ABI
  // version), the development files package (e.g., libfoo-dev), the
  // documentation files package (e.g., libfoo-doc), the debug symbols
  // package (e.g., libfoo1-dbg), and the architecture-independent files
  // (e.g., libfoo1-common). All the packages except -dev are optional
  // and there is quite a bit of variability here. Here are a few examples:
  //
  // libz3-4 libz3-dev
  //
  // libssl1.1 libssl-dev libssl-doc
  // libssl3 libssl-dev libssl-doc
  //
  // libcurl4 libcurl4-doc libcurl4-openssl-dev
  // libcurl3-gnutls libcurl4-gnutls-dev         (yes, 3 and 4)
  //
  // Based on that, it seems our best bet when trying to automatically map our
  // library package name to Debian package names is to go for the -dev
  // package first and figure out the shared library package from that based
  // on the fact that the -dev package should have the == dependency on the
  // shared library package with the same version and its name should normally
  // start with the -dev package's stem.
  //
  // For executable packages there is normally no -dev packages but -dbg,
  // -doc, and -common are plausible.
  //
  // The format of the debian-name (or alike) manifest value value is a comma-
  // separated list of one or more package groups:
  //
  // <package-group> [, <package-group>...]
  //
  // Where each <package-group> is the space-separated list of one or more
  // package names:
  //
  // <package-name> [  <package-name>...]
  //
  // All the packages in the group should be "package components" (for the
  // lack of a better term) of the same "logical package", such as -dev, -doc,
  // -common packages. They normally have the same version.
  //
  // The first group is called the main group and the first package in the
  // group is called the main package.
  //
  // We allow/recommend specifying the -dev package instead of the main
  // package for libraries (the name starts with lib), seeing that we are
  // capable of detecting the main package automatically. If the library name
  // happens to end with -dev (which poses an ambiguity), then the -dev
  // package should be specified explicitly as the second package to
  // disambiguate this situation (if a non-library name happened to start with
  // lib and end with -dev, well, you are out of luck, I guess).
  //
  // Note also that for now we treat all the packages from the non-main groups
  // as extras but in the future we may decide to sort them out like the main
  // group (see parse_name_value() for details).
  //
  struct system_package_status_debian: system_package_status
  {
    string main;
    string dev;
    string doc;
    string dbg;
    string common;
    strings extras;

    // The `apt-cache policy` output.
    //
    struct package_policy
    {
      string name;
      string installed_version; // Empty if none.
      string candidate_version; // Empty if none and no installed_version.

      explicit
      package_policy (string n): name (move (n)) {}
    };

    vector<package_policy> package_policies;
    size_t package_policies_main = 0; // Size of the main group.

    explicit
    system_package_status_debian (string m, string d = {})
        : main (move (m)), dev (move (d))
    {
      assert (!main.empty () || !dev.empty ());
    }

    system_package_status_debian () = default;
  };

  class system_package_manager_debian: public system_package_manager
  {
  public:
    virtual optional<const system_package_status*>
    pkg_status (const package_name&, const available_packages*) override;

    virtual void
    pkg_install (const vector<package_name>&) override;

  public:
    // Note: expects os_release::name_id to be "debian" or os_release::like_id
    // to contain "debian".
    //
    using system_package_manager::system_package_manager;

    // Implementation details exposed for testing (see definitions for
    // documentation).
    //
  public:
    using package_status = system_package_status_debian;
    using package_policy = package_status::package_policy;

    void
    apt_cache_policy (vector<package_policy>&, size_t = 0);

    string
    apt_cache_show (const string&, const string&);

    void
    apt_get_update ();

    void
    apt_get_install (const strings&);

    pair<cstrings, const process_path&>
    apt_get_common (const char*);

    static package_status
    parse_name_value (const string&, bool, bool);

    static string
    main_from_dev (const string&, const string&, const string&);

    // If simulate is not NULL, then instead of executing the actual apt-cache
    // and apt-get commands simulate their execution: (1) for apt-cache by
    // printing their command lines and reading the results from files
    // specified in the below apt_cache_* maps and (2) for apt-get by printing
    // their command lines and failing if requested.
    //
    // In the (1) case if the corresponding map entry does not exist or the
    // path is empty, then act as if the specified package/version is
    // unknown. If the path is special "-" then read from stdin. For apt-cache
    // different post-fetch and (for policy) post-install results can be
    // specified (if the result is not found in one of the later maps, the
    // previous map is used as a fallback). Note that the keys in the
    // apt_cache_policy_* maps are the package sets and the corresponding
    // result file is expected to contain (or not) the results for all of
    // them. See apt_cache_policy() and apt_cache_show() implementations for
    // details on the expected results.
    //
    struct simulation
    {
      std::map<strings, path> apt_cache_policy_;
      std::map<strings, path> apt_cache_policy_fetched_;
      std::map<strings, path> apt_cache_policy_installed_;

      std::map<pair<string, string>, path> apt_cache_show_;
      std::map<pair<string, string>, path> apt_cache_show_fetched_;

      bool apt_get_update_fail_ = false;
      bool apt_get_install_fail_ = false;
    };

    const simulation* simulate_ = nullptr;

  protected:
    bool fetched_ = false;   // True if already fetched metadata.
    bool installed_ = false; // True if already installed.

    std::map<package_name, optional<system_package_status_debian>> status_cache_;
  };
}

#endif // BPKG_SYSTEM_PACKAGE_MANAGER_DEBIAN_HXX
