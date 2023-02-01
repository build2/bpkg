// file      : bpkg/system-repository.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-repository.hxx>

namespace bpkg
{
  const version& system_repository::
  insert (const package_name& name,
          const version& v,
          bool authoritative,
          const system_package_status* s)
  {
    auto p (map_.emplace (name, system_package {v, authoritative, s}));

    if (!p.second)
    {
      system_package& sp (p.first->second);

      // We should not override authoritative information.
      //
      assert (!(authoritative && sp.authoritative));

      if (authoritative >= sp.authoritative)
      {
        sp.authoritative = authoritative;
        sp.version = v;
        sp.system_status = s;
      }
    }

    return p.first->second.version;
  }
}
