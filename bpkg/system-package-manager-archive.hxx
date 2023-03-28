// file      : bpkg/system-package-manager-archive.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SYSTEM_PACKAGE_MANAGER_ARCHIVE_HXX
#define BPKG_SYSTEM_PACKAGE_MANAGER_ARCHIVE_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/system-package-manager.hxx>

namespace bpkg
{
  // The system package manager implementation for the installation archive
  // packages, production only.
  //
  class system_package_manager_archive: public system_package_manager
  {
  public:
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

    virtual optional<const system_package_status*>
    status (const package_name&, const available_packages*) override;

    virtual void
    install (const vector<package_name>&) override;

  public:
    // Note: options can only be NULL when testing functions that don't need
    // them.
    //
    system_package_manager_archive (bpkg::os_release&&,
                                    const target_triplet& host,
                                    string arch,
                                    optional<bool> progress,
                                    const pkg_bindist_options*);

  protected:
    // Only for production.
    //
    const pkg_bindist_options* ops = nullptr;
    target_triplet target;
  };
}

#endif // BPKG_SYSTEM_PACKAGE_MANAGER_ARCHIVE_HXX
