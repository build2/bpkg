// file      : bpkg/system-package-manager-fedora.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SYSTEM_PACKAGE_MANAGER_FEDORA_HXX
#define BPKG_SYSTEM_PACKAGE_MANAGER_FEDORA_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/system-package-manager.hxx>

namespace bpkg
{
  // The system package manager implementation for Fedora and alike (Red Hat
  // Enterprise Linux, CentOS, etc) using the DNF frontend.
  //
  // For background, a library in Fedora is normally split up into several
  // packages: the shared library package (e.g., libfoo), the development
  // files package (e.g., libfoo-devel), the static library package (e.g.,
  // libfoo-static; may also be placed into the -devel package), the
  // documentation files package (e.g., libfoo-doc), the debug symbols and
  // source files packages (e.g., libfoo-debuginfo and libfoo-debugsource),
  // and the common or architecture-independent files (e.g., libfoo-common).
  // All the packages except -devel are optional and there is quite a bit of
  // variability here. In particular, the lib prefix in libfoo is not a
  // requirement (unlike in Debian) and is normally present only if upstream
  // name has it (see some examples below).
  //
  // For mixed packages which include both applications and libraries, the
  // shared library package normally have the -libs suffix (e.g., foo-libs).
  //
  // A package name may also include an upstream version based suffix if
  // multiple versions of the package can be installed simultaneously (e.g.,
  // libfoo1.1 libfoo1.1-devel, libfoo2 libfoo2-devel).
  //
  // Terminology-wise, the term "base package" (sometime also "main package")
  // normally refers to either the application or shared library package (as
  // decided by the package maintainer in the spec file) with the suffixed
  // packages (-devel, -doc, etc) called "subpackages".
  //
  // Here are a few examples:
  //
  // libpq libpq-devel
  //
  // zlib zlib-devel zlib-static
  //
  // xerces-c xerces-c-devel xerces-c-doc
  //
  // libsigc++20 libsigc++20-devel libsigc++20-doc
  // libsigc++30 libsigc++30-devel libsigc++30-doc
  //
  // icu libicu libicu-devel libicu-doc
  //
  // openssl openssl-devel openssl-libs
  //
  // curl libcurl libcurl-devel
  //
  // sqlite sqlite-libs sqlite-devel sqlite-doc
  //
  // community-mysql community-mysql-libs community-mysql-devel
  // community-mysql-common community-mysql-server
  //
  // Based on that, it seems our best bet when trying to automatically map our
  // library package name to Fedora package names is to go for the -devel
  // package first and figure out the shared library package from that based
  // on the fact that the -devel package should have the == dependency on the
  // shared library package with the same version and its name should normally
  // start with the -devel package's stem and be potentially followed with the
  // -libs suffix. Failed to find the -devel package, we may re-try but now
  // using the project name instead of the package name (see, for example,
  // openssl, sqlite).
  //
  // For application packages there is normally no -devel packages but
  // -debug*, -doc, and -common are plausible.
  //
  // The format of the fedora-name (or alike) manifest value value is a comma-
  // separated list of one or more package groups:
  //
  // <package-group> [, <package-group>...]
  //
  // Where each <package-group> is the space-separated list of one or more
  // package names:
  //
  // <package-name> [ <package-name>...]
  //
  // All the packages in the group should belong to the same "logical
  // package", such as -devel, -doc, -common packages. They normally have the
  // same version.
  //
  // The first group is called the main group and the first package in the
  // group is called the main package. Note that all the groups are consumed
  // (installed) but only the main group is produced (packaged).
  //
  // (Note that above we use the term "logical package" instead of "base
  // package" since the main package may not be the base package, for example
  // being the -libs subpackage.)
  //
  // We allow/recommend specifying the -devel package instead of the main
  // package for libraries (the bpkg package name starts with lib), seeing
  // that we are capable of detecting the main package automatically. If the
  // library name happens to end with -devel (which poses an ambiguity), then
  // the -devel package should be specified explicitly as the second package
  // to disambiguate this situation (if a non-library name happened to start
  // with lib and end with -devel, well, you are out of luck, I guess).
  //
  // Note also that for now we treat all the packages from the non-main groups
  // as extras but in the future we may decide to sort them out like the main
  // group (see parse_name_value() for details).
  //
  // The Fedora package version has the [<epoch>:]<version>-<release> form
  // where the parts correspond to the Epoch (optional upstream versioning
  // scheme), Version (upstream version), and Release (Fedora's package
  // revision) RPM tags (see the Fedora package Versioning Guidelines and RPM
  // tags documentation for details). If no explicit mapping to bpkg version
  // is specified with the fedora-to-downstream-version manifest values (or
  // alike), then we fallback to using the <version> part as bpkg version. If
  // explicit mapping is specified, then we match it against the
  // [<epoch>:]<version> parts ignoring <release>.
  //
  struct system_package_status_fedora: system_package_status
  {
    string main;
    string devel;
    string doc;
    string debuginfo;
    string debugsource;
    string common;
    strings extras;

    string devel_fallback; // Fallback based on project name.

    // @@ Rename. package_info?
    //
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
    system_package_status_fedora (string m, string d = {})
        : main (move (m)), devel (move (d))
    {
      assert (!main.empty () || !devel.empty ());
    }

    system_package_status_fedora () = default;
  };

  class system_package_manager_fedora: public system_package_manager
  {
  public:
    virtual optional<const system_package_status*>
    pkg_status (const package_name&, const available_packages*) override;

    virtual void
    pkg_install (const vector<package_name>&) override;

  public:
    // Note: expects os_release::name_id to be "fedora" or os_release::like_id
    // to contain "fedora".
    //
    using system_package_manager::system_package_manager;

    // Implementation details exposed for testing (see definitions for
    // documentation).
    //
  public:
    using package_status = system_package_status_fedora;
    using package_policy = package_status::package_policy;

    void
    dnf_list (vector<package_policy>&, size_t = 0);

    vector<pair<string, string>>
    dnf_repoquery_requires (const string&, const string&);

    static package_status
    parse_name_value (const string&, bool, bool, bool);

    // @@ TODO
    //
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
#if 0
      std::map<strings, path> apt_cache_policy_;
      std::map<strings, path> apt_cache_policy_fetched_;
      std::map<strings, path> apt_cache_policy_installed_;

      std::map<pair<string, string>, path> apt_cache_show_;
      std::map<pair<string, string>, path> apt_cache_show_fetched_;

      bool apt_get_update_fail_ = false;
      bool apt_get_install_fail_ = false;
#endif
    };

    const simulation* simulate_ = nullptr;

  protected:
    bool fetched_ = false;   // True if already fetched metadata.
    bool installed_ = false; // True if already installed.

    std::map<package_name, optional<system_package_status_fedora>> status_cache_;
  };
}

#endif // BPKG_SYSTEM_PACKAGE_MANAGER_FEDORA_HXX
