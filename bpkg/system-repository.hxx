// file      : bpkg/system-repository.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SYSTEM_REPOSITORY_HXX
#define BPKG_SYSTEM_REPOSITORY_HXX

#include <map>

#include <libbpkg/manifest.hxx>     // version
#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

namespace bpkg
{
  struct system_package_status; // <bpkg/system-package-manager.hxx>

  // A map of discovered system package versions. The information can be
  // authoritative (i.e., it was provided by the user or auto-discovered
  // on this run) or non-authoritative (i.e., comes from selected packages
  // that are present in the database; in a sence it was authoritative but
  // on some previous run.
  //
  // Note that in our model we assume that once an authoritative version has
  // been discovered, it does not change (on this run; see caching logic in
  // available package).
  //
  struct system_package
  {
    using version_type = bpkg::version;

    version_type version;
    bool authoritative;

    // If the information is authoritative then this member indicates whether
    // the version came from the system package manager (not NULL) or
    // user/fallback (NULL).
    //
    const system_package_status* system_status;
  };

  class system_repository
  {
  public:
    const version&
    insert (const package_name& name,
            const version&,
            bool authoritative,
            const system_package_status* = nullptr);

    const system_package*
    find (const package_name& name) const
    {
      auto i (map_.find (name));
      return i != map_.end () ? &i->second : nullptr;
    }

  private:
    std::map<package_name, system_package> map_;
  };
}

#endif // BPKG_SYSTEM_REPOSITORY_HXX
