// file      : bpkg/system-repository.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-repository.hxx>

namespace bpkg
{
  const system_package_versions& system_repository::
  insert (const package_name& name,
          const system_package_versions& vs,
          bool authoritative)
  {
    assert (!vs.empty ());

    auto p (map_.emplace (name, system_package {vs, authoritative}));

    if (!p.second)
    {
      system_package& sp (p.first->second);

      // We should not override authoritative information.
      //
      assert (!(authoritative && sp.authoritative));

      if (authoritative >= sp.authoritative)
      {
        sp.authoritative = authoritative;
        sp.versions = vs;
      }
    }

    return p.first->second.versions;
  }
}
