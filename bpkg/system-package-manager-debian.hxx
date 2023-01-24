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
  // For a manual mapping we will require the user to always specify the
  // shared library package and the -dev package names explicitly.
  //
  // For executable packages there is normally no -dev packages but -dbg,
  // -doc, and -common are plausible.
  //
  class system_package_status_debian: public system_package_status
  {
  public:
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
      reference_wrapper<const string> name;

      string installed_version; // Empty if none.
      string candidate_version; // Empty if none and no installed_version.

      explicit
      package_policy (const string& n): name (n) {}
    };

    vector<package_policy> package_policies;
    size_t package_policies_main = 0; // Size of the main group.

    explicit
    system_package_status_debian (string m, string d = {})
        : main (move (m)), dev (move (d))
    {
      assert (!main.empty () || !dev.empty ());
    }
  };

  class system_package_manager_debian: public system_package_manager
  {
  public:
    virtual optional<const system_package_status*>
    pkg_status (const package_name&,
                const available_packages*,
                bool install,
                bool fetch) override;

    virtual void
    pkg_install (const vector<package_name>&,
                 bool install) override;

  public:
    // Note: expects os_release::name_id to be "debian" or os_release::like_id
    // to contain "debian".
    //
    explicit
    system_package_manager_debian (const common_options& co, os_release&& osr)
        : system_package_manager (co, move (osr)) {}

  protected:
    bool fetched_ = false;   // True if already fetched metadata.
    bool installed_ = false; // True if already installed.

    // @@ Don't need unique_ptr/polymorphism.
    //
    std::map<package_name,
             unique_ptr<system_package_status_debian>> status_cache_;
  };
}

#endif // BPKG_SYSTEM_PACKAGE_MANAGER_DEBIAN_HXX
