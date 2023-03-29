// file      : bpkg/system-package-manager.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SYSTEM_PACKAGE_MANAGER_HXX
#define BPKG_SYSTEM_PACKAGE_MANAGER_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <libbutl/path-map.hxx>
#include <libbutl/host-os-release.hxx>

#include <bpkg/package.hxx>
#include <bpkg/common-options.hxx>

namespace bpkg
{
  using os_release = butl::os_release;

  // The system/distribution package manager interface. Used by both pkg-build
  // (to query and install system packages) and by pkg-bindist (to generate
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
  // due to them being installed into the same location (e.g., /usr/include).
  // The same holds for executables, which are installed into the same
  // location (e.g., /usr/bin).
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
  struct system_package_status
  {
    // Downstream (as in, bpkg package) version.
    //
    bpkg::version version;

    // System (as in, distribution) package name and version for diagnostics.
    //
    // Note that this status may represent multiple system packages (for
    // example, libfoo and libfoo-dev) and here we have only the
    // main/representative package name (for example, libfoo).
    //
    string system_name;
    string system_version;

    // The system package can be either "available already installed",
    // "available partially installed" (for example, libfoo but not
    // libfoo-dev is installed) or "available not yet installed".
    //
    enum status_type {installed, partially_installed, not_installed};

    status_type status = not_installed;
  };

  // As mentioned above the system package manager API has two parts:
  // consumption (status() and install()) and production (generate()) and a
  // particular implementation may only implement one, the other, or both. If
  // a particular part is not implemented, then the correponding make_*()
  // function below should never return an instance of such a system package
  // manager.
  //
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
    // is available from the system package manager. Note that NULL is also
    // returned if no fully installed package is available from the system and
    // package installation is not enabled (see the constructor below).
    //
    // Note also that the implementation is expected to issue appropriate
    // progress and diagnostics if fetching package metadata (again see the
    // constructor below).
    //
    virtual optional<const system_package_status*>
    status (const package_name&, const available_packages*) = 0;

    // Install the specified subset of the previously-queried packages.
    // Should only be called if installation is enabled (see the constructor
    // below).
    //
    // Note that this function should be called only once after the final set
    // of the required system packages has been determined. And the specified
    // subset should contain all the selected packages, including the already
    // fully installed. This allows the implementation to merge and de-
    // duplicate the system package set to be installed (since some bpkg
    // packages may be mapped to the same system package), perform post-
    // installation verifications (such as making sure the versions of already
    // installed packages have not changed due to upgrades), change properties
    // of already installed packages (e.g., mark them as manually installed in
    // Debian), etc.
    //
    // Note also that the implementation is expected to issue appropriate
    // progress and diagnostics.
    //
    virtual void
    install (const vector<package_name>&) = 0;

    // Generate a binary distribution package. See the pkg-bindist(1) man page
    // for background and the pkg_bindist() function implementation for
    // details. The recursive_full argument corresponds to the --recursive
    // auto (present false) and full (present true) modes.
    //
    // The available packages are loaded for all the packages in pkgs and
    // deps. For non-system packages (so for all in pkgs) there is always a
    // single available package that corresponds to the selected package. The
    // out_root is only set for packages in pkgs. Note also that all the
    // packages in pkgs and deps are guaranteed to belong to the same build
    // configuration (as opposed to being spread over multiple linked
    // configurations). Its absolute path is bassed in cfg_dir.
    //
    // The passed package manifest corresponds to the first package in pkgs
    // (normally used as a source of additional package metadata such as
    // summary, emails, urls, etc).
    //
    // The passed package type corresponds to the first package in pkgs while
    // the languages -- to all the packages in pkgs plus, in the recursive
    // mode, to all the non-system dependencies. In other words, the languages
    // list contains every language that is used by anything that ends up in
    // the package.
    //
    // Return the list of paths to binary packages and any other associated
    // files (build metadata, etc) that could be useful for their consumption.
    // Each returned file has a distribution-specific type that classifies it.
    // If the result is empty, assume the prepare-only mode (or similar) with
    // appropriate result diagnostics having been already issued.
    //
    // Note that this function may be called multiple times in the
    // --recursive=separate mode. In this case the first argument indicates
    // whether this is the first call (can be used, for example, to adjust the
    // --wipe-output semantics).
    //
    struct package
    {
      shared_ptr<selected_package> selected;
      available_packages           available;
      dir_path                     out_root; // Absolute and normalized.
    };

    using packages = vector<package>;

    struct binary_file
    {
      string     type;
      bpkg::path path;
      string     system_name; // Empty if not applicable.
    };

    struct binary_files: public vector<binary_file>
    {
      string system_version; // Empty if not applicable.
    };

    virtual binary_files
    generate (const packages& pkgs,
              const packages& deps,
              const strings& vars,
              const dir_path& cfg_dir,
              const package_manifest&,
              const string& type,
              const small_vector<language, 1>&,
              optional<bool> recursive_full,
              bool first) = 0;

  public:
    bpkg::os_release os_release;
    target_triplet   host;
    string           arch; // Architecture in system package manager spelling.

    // Consumption constructor.
    //
    // If install is true, then enable package installation.
    //
    // If fetch is false, then do not re-fetch the system package repository
    // metadata (that is, available packages/versions) before querying for the
    // available version of the not yet installed or partially installed
    // packages.
    //
    // If fetch timeout (in seconds) is specified, then use it for all the
    // underlying network operations.
    //
    system_package_manager (bpkg::os_release&& osr,
                            const target_triplet& h,
                            string a,
                            optional<bool> progress,
                            optional<size_t> fetch_timeout,
                            bool install,
                            bool fetch,
                            bool yes,
                            string sudo)
        : os_release (move (osr)),
          host (h),
          arch (move (a)),
          progress_ (progress),
          fetch_timeout_ (fetch_timeout),
          install_ (install),
          fetch_ (fetch),
          yes_ (yes),
          sudo_ (sudo != "false" ? move (sudo) : string ()) {}

    // Production constructor.
    //
    system_package_manager (bpkg::os_release&& osr,
                            const target_triplet& h,
                            string a,
                            optional<bool> progress)
        : os_release (move (osr)),
          host (h),
          arch (move (a)),
          progress_ (progress),
          install_ (false),
          fetch_ (false),
          yes_ (false) {}

    virtual
    ~system_package_manager ();

    // Implementation details.
    //
  public:
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
    // the values that are equal or less than the specified version_id
    // (include the value with the absent <version>). In a sense, absent
    // <version> is treated as a 0 semver-like version.
    //
    // If no value is found then repeat the above process for every like_ids
    // entry (from left to right) instead of name_id with version_id equal 0.
    //
    // If still no value is found, then return empty list (in which case the
    // caller may choose to fallback to the downstream package name or do
    // something more elaborate, like translate version_id to one of the
    // like_id's version and try that).
    //
    // Note that multiple -name values per same distribution can be returned
    // as, for example, for the following distribution values:
    //
    // debian_10-name: libcurl4 libcurl4-doc libcurl4-openssl-dev
    // debian_10-name: libcurl3-gnutls libcurl4-gnutls-dev    (yes, 3 and 4)
    //
    // The <distribution> value in the <name>_0 form is the special "non-
    // native" name mapping. If the native argument is false, then such a
    // mapping is preferred over any other mapping. If it is true, then such a
    // mapping is ignored. The purpose of this special value is to allow
    // specifying different package names for production compared to
    // consumption. Note, however, that such a deviation may make it
    // impossible to use native and non-native binary packages
    // interchangeably, for example, to satisfy dependencies.
    //
    // Note also that the values are returned in the "override order", that is
    // from the newest package version to oldest and then from the highest
    // distribution version to lowest.
    //
    static strings
    system_package_names (const available_packages&,
                          const string& name_id,
                          const string& version_id,
                          const vector<string>& like_ids,
                          bool native);

    // Given the available package and the repository fragment it belongs to,
    // return the system package version as mapped by one of the
    // <distribution>-version values.
    //
    // The rest of the arguments as well as the overalls semantics is the same
    // as in system_package_names() above. That is, first consider
    // <distribution>-version values corresponding to name_id. If none match,
    // then repeat the above process for every like_ids entry with version_id
    // equal 0. If still no match, then return nullopt (in which case the
    // caller may choose to fallback to the upstream/bpkg package version or
    // do something more elaborate).
    //
    // Note that lazy_shared_ptr<repository_fragment> is used only for
    // diagnostics and conveys the database the available package object
    // belongs to.
    //
    static optional<string>
    system_package_version (const shared_ptr<available_package>&,
                            const lazy_shared_ptr<repository_fragment>&,
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
    // package version or do something more elaborate).
    //
    static optional<version>
    downstream_package_version (const string& system_version,
                                const available_packages&,
                                const string& name_id,
                                const string& version_id,
                                const vector<string>& like_ids);

    // Return the map of filesystem entries (files and symlinks) that would be
    // installed for the specified packages with the specified configuration
    // variables.
    //
    // In essence, this function runs:
    //
    // b --dry-run --quiet <vars> !config.install.scope=<scope>
    //   !config.install.manifest=- install: <pkgs>
    //
    // And converts the printed installation manifest into the path map.
    //
    // Note that this function prints an appropriate progress indicator since
    // even in the dry-run mode it may take some time (see the --dry-run
    // option documentation for details).
    //
    struct installed_entry
    {
      string mode;                                     // Empty if symlink.
      const pair<const path, installed_entry>* target; // Target if symlink.
    };

    class installed_entry_map: public butl::path_map<installed_entry>
    {
    public:
      // Return true if there are filesystem entries in the specified
      // directory or its subdirectories.
      //
      bool
      contains_sub (const dir_path& d)
      {
        auto p (find_sub (d));
        return p.first != p.second;
      }
    };

    installed_entry_map
    installed_entries (const common_options&,
                       const packages& pkgs,
                       const strings& vars,
                       const string& scope);

  protected:
    optional<bool>   progress_;      // --[no]-progress (see also stderr_term)
    optional<size_t> fetch_timeout_; // --fetch-timeout

    // The --sys-* option values.
    //
    bool install_;
    bool fetch_;
    bool yes_;
    string sudo_;
  };

  // Create a package manager instance corresponding to the specified host
  // target triplet as well as optional distribution package manager name and
  // architecture. If name is empty, return NULL if there is no support for
  // this platform. If architecture is empty, then derive it automatically
  // from the host target triplet. Currently recognized names:
  //
  //   debian  -- Debian and alike (Ubuntu, etc) using the APT frontend.
  //   fedora  -- Fedora and alike (RHEL, Centos, etc) using the DNF frontend.
  //   archive -- Installation archive, any platform, production only.
  //
  // Note: the name can be used to select an alternative package manager
  // implementation on platforms that support multiple.
  //
  unique_ptr<system_package_manager>
  make_consumption_system_package_manager (const common_options&,
                                           const target_triplet&,
                                           const string& name,
                                           const string& arch,
                                           bool install,
                                           bool fetch,
                                           bool yes,
                                           const string& sudo);

  // Create for production. The second half of the result is the effective
  // distribution name.
  //
  // Note that the reference to options is expected to outlive the returned
  // instance.
  //
  class pkg_bindist_options;

  pair<unique_ptr<system_package_manager>, string>
  make_production_system_package_manager (const pkg_bindist_options&,
                                          const target_triplet&,
                                          const string& name,
                                          const string& arch);
}

#endif // BPKG_SYSTEM_PACKAGE_MANAGER_HXX
