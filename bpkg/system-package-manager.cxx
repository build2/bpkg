// file      : bpkg/system-package-manager.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager.hxx>

#include <bpkg/diagnostics.hxx>

namespace bpkg
{
  system_package_manager::
  ~system_package_manager ()
  {
    // vtable
  }

  unique_ptr<system_package_manager>
  make_system_package_manager (const target_triplet& host,
                               const string& type)
  {
    unique_ptr<system_package_manager> r;

    if (r == nullptr)
    {
      if (!type.empty ())
        fail << "unsupported package manager type '" << type << "' for host "
             << host;
    }

    return r;
  }
}
