// file      : bpkg/system-package-manager-fedora.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SYSTEM_PACKAGE_MANAGER_FEDORA_HXX
#define BPKG_SYSTEM_PACKAGE_MANAGER_FEDORA_HXX

#include <map>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/system-package-manager.hxx>

namespace bpkg
{
  // The system package manager implementation for Fedora and alike (Red Hat
  // Enterprise Linux, CentOS, etc) using the DNF frontend.
  //
  // NOTE: THE BELOW DESCRIPTION IS ALSO REPRODUCED IN THE BPKG MANUAL.
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
  // shared library package normally has the -libs suffix (e.g., foo-libs).
  // Such packages may have separate -debuginfo packages for applications and
  // libraries (e.g. openssl-debuginfo and openssl-libs-debuginfo).
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
  // catch-devel
  //
  // eigen3-devel eigen3-doc
  //
  // xerces-c xerces-c-devel xerces-c-doc
  //
  // libsigc++20 libsigc++20-devel libsigc++20-doc
  // libsigc++30 libsigc++30-devel libsigc++30-doc
  //
  // icu libicu libicu-devel libicu-doc
  //
  // openssl openssl-libs openssl-devel openssl-static
  // openssl1.1 openssl1.1-devel
  //
  // curl libcurl libcurl-devel
  //
  // sqlite sqlite-libs sqlite-devel sqlite-doc
  //
  // community-mysql community-mysql-libs community-mysql-devel
  // community-mysql-common community-mysql-server
  //
  // ncurses ncurses-libs ncurses-c++-libs ncurses-devel ncurses-static
  //
  // keyutils keyutils-libs keyutils-libs-devel
  //
  // Note that while we support arbitrary -debug* sub-package names for
  // consumption, we only generate <main-package>-debug*.
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
  // The format of the fedora-name (or alike) manifest value is a comma-
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
  // revision) RPM tags (see the Fedora Package Versioning Guidelines and RPM
  // tags documentation for details). If no explicit mapping to the bpkg
  // version is specified with the fedora-to-downstream-version (or alike)
  // manifest values or none match, then we fallback to using the <version>
  // part as the bpkg version. If explicit mapping is specified, then we match
  // it against the [<epoch>:]<version> parts ignoring <release>.
  //
  struct system_package_status_fedora: system_package_status
  {
    string main;
    string devel;
    string static_;
    string doc;
    string debuginfo;
    string debugsource;
    string common;
    strings extras;

    string fallback; // Fallback for main/devel package based on project name.

    // The `dnf list` output.
    //
    struct package_info
    {
      string name;
      string installed_version; // Empty if none.
      string candidate_version; // Empty if none and no installed_version.

      // The architecture of the installed/candidate package version. Can only
      // be the host architecture or noarch (so it could have been bool but
      // it's more convenient to have the actual name).
      //
      // Note that in Fedora the same package version can be available for
      // multiple architectures or be architecture-independent. For example:
      //
      // dbus-libs-1:1.12.22-1.fc35.i686
      // dbus-libs-1:1.12.22-1.fc35.x86_64
      // dbus-common-1:1.12.22-1.fc35.noarch
      // code-insiders-1.75.0-1675123170.el7.armv7hl
      // code-insiders-1.75.0-1675123170.el7.aarch64
      // code-insiders-1.75.0-1675123170.el7.x86_64
      //
      // Thus, for a package query we normally need to qualify the package
      // with the architecture suffix or filter the query result, normally
      // skipping packages for architectures other than the host architecture.
      //
      string installed_arch;
      string candidate_arch;

      explicit
      package_info (string n): name (move (n)) {}
    };

    vector<package_info> package_infos;
    size_t package_infos_main = 0; // Size of the main group.

    explicit
    system_package_status_fedora (string m, string d = {}, string f = {})
        : main (move (m)), devel (move (d)), fallback (move (f))
    {
      assert (!main.empty () || !devel.empty ());
    }

    system_package_status_fedora () = default;
  };

  class system_package_manager_fedora: public system_package_manager
  {
  public:
    virtual optional<const system_package_status*>
    status (const package_name&, const available_packages*) override;

    virtual void
    install (const vector<package_name>&) override;

    virtual binary_files
    generate (const packages&,
              const packages&,
              const strings&,
              const dir_path&,
              const package_manifest&,
              const string&,
              const small_vector<language, 1>&,
              optional<bool>,
              bool) override;

  public:
    // Expect os_release::name_id to be "fedora" or os_release::like_ids to
    // contain "fedora".
    //
    system_package_manager_fedora (bpkg::os_release&& osr,
                                   const target_triplet& h,
                                   string a,
                                   optional<bool> progress,
                                   optional<size_t> fetch_timeout,
                                   bool install,
                                   bool fetch,
                                   bool yes,
                                   string sudo)
        : system_package_manager (move (osr),
                                  h,
                                  a.empty () ? arch_from_target (h) : move (a),
                                  progress,
                                  fetch_timeout,
                                  install,
                                  fetch,
                                  yes,
                                  move (sudo)) {}

    // Note: options can only be NULL when testing functions that don't need
    // them.
    //
    system_package_manager_fedora (bpkg::os_release&& osr,
                                   const target_triplet& h,
                                   string a,
                                   optional<bool> progress,
                                   const pkg_bindist_options* ops)
        : system_package_manager (move (osr),
                                  h,
                                  a.empty () ? arch_from_target (h) : move (a),
                                  progress),
          ops_ (ops) {}

    // Implementation details exposed for testing (see definitions for
    // documentation).
    //
  public:
    using package_status = system_package_status_fedora;
    using package_info = package_status::package_info;

    void
    dnf_list (vector<package_info>&, size_t = 0);

    vector<pair<string, string>>
    dnf_repoquery_requires (const string&, const string&, const string&, bool);

    void
    dnf_makecache ();

    void
    dnf_install (const strings&);

    void
    dnf_mark_install (const strings&);

    pair<cstrings, const process_path&>
    dnf_common (const char*,
                optional<size_t> fetch_timeout,
                strings& args_storage);

    static package_status
    parse_name_value (const string&, const string&, bool, bool, bool);

    static string
    main_from_devel (const string&,
                     const string&,
                     const vector<pair<string, string>>&);

    static string
    arch_from_target (const target_triplet&);

    package_status
    map_package (const package_name&,
                 const version&,
                 const available_packages&) const;

    static strings
    rpm_eval (const cstrings& opts, const cstrings& expressions);

    // If simulate is not NULL, then instead of executing the actual dnf
    // commands simulate their execution: (1) for `dnf list` and `dnf
    // repoquery --requires` by printing their command lines and reading the
    // results from files specified in the below dnf_* maps and (2) for `dnf
    // makecache`, `dnf install`, and `dnf mark install` by printing their
    // command lines and failing if requested.
    //
    // In the (1) case if the corresponding map entry does not exist or the
    // path is empty, then act as if the specified package/version is
    // unknown. If the path is special "-" then read from stdin. For `dnf
    // list` and `dnf repoquery --requires` different post-fetch and (for the
    // former) post-install results can be specified (if the result is not
    // found in one of the later maps, the previous map is used as a
    // fallback). Note that the keys in the dnf_list_* maps are the package
    // sets and the corresponding result file is expected to contain (or not)
    // the results for all of them. See dnf_list() and
    // dnf_repoquery_requires() implementations for details on the expected
    // results.
    //
    struct simulation
    {
      std::map<strings, path> dnf_list_;
      std::map<strings, path> dnf_list_fetched_;
      std::map<strings, path> dnf_list_installed_;

      struct package
      {
        string name;
        string version;
        string arch;
        bool installed;

        bool
        operator< (const package& p) const
        {
          if (int r = name.compare (p.name))
            return r < 0;

          if (int r = version.compare (p.version))
            return r < 0;

          if (int r = arch.compare (p.arch))
            return r < 0;

          return installed < p.installed;
        }
      };

      std::map<package, path> dnf_repoquery_requires_;
      std::map<package, path> dnf_repoquery_requires_fetched_;

      bool dnf_makecache_fail_ = false;
      bool dnf_install_fail_ = false;
      bool dnf_mark_install_fail_ = false;
    };

    const simulation* simulate_ = nullptr;

  private:
    optional<system_package_status_fedora>
    status (const package_name&, const available_packages&);

  private:
    bool fetched_ = false;   // True if already fetched metadata.
    bool installed_ = false; // True if already installed.

    std::map<package_name, optional<system_package_status_fedora>> status_cache_;

    const pkg_bindist_options* ops_ = nullptr; // Only for production.
  };
}

#endif // BPKG_SYSTEM_PACKAGE_MANAGER_FEDORA_HXX
