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
  // Note that in our model we assume that once an authoritative versions have
  // been discovered, they does not change (on this run; see caching logic in
  // available package).
  //
  using system_package_versions = small_vector<version, 2>;

  struct system_package
  {
    system_package_versions versions;
    bool authoritative;
  };

  class system_repository
  {
  public:
    // @@ Add move-insertion overloads (system_package_versions&& and
    //    version&&)?
    //

    // Note: the system package versions are assumed to be provided in the
    // preference descending order.
    //
    const system_package_versions&
    insert (const package_name& name,
            const system_package_versions&,
            bool authoritative);

    const system_package_versions&
    insert (const package_name& n, const version& v, bool a)
    {
      return insert (n, system_package_versions ({v}), a);
    }

    const system_package*
    find (const package_name& name)
    {
      auto i (map_.find (name));
      return i != map_.end () ? &i->second : nullptr;
    }

  private:
    std::map<package_name, system_package> map_;
  };
}

#endif // BPKG_SYSTEM_REPOSITORY_HXX
