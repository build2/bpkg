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
  // A map of discovered system package versions. The information can be
  // authoritative (i.e., it was provided by the user or auto-discovered
  // on this run) or non-authoritative (i.e., comes from selected_packages
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
  };

  class system_repository_type
  {
  public:
    const version&
    insert (const package_name& name, const version&, bool authoritative);

    const system_package*
    find (const package_name& name)
    {
      auto i (map_.find (name));
      return i != map_.end () ? &i->second : nullptr;
    }

  private:
    std::map<package_name, system_package> map_;
  };

  extern system_repository_type system_repository;
}

#endif // BPKG_SYSTEM_REPOSITORY_HXX
