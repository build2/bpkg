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
                               const string& name)
  {
    unique_ptr<system_package_manager> r;

    if (optional<os_release> osr = host_os_release (host))
    {
      if (host.class_ == "linux")
      {
        if (osr->name_id == "debian" ||
            osr->name_id == "ubuntu" ||
            find_if (osr->like_ids.begin (), osr->like_ids.end (),
                     [] (const string& n)
                     {
                       return n == "debian" || n == "ubuntu";
                     }) != osr->like_ids.end ())
        {
          // @@ TODO: verify name if specified.

          //r.reset (new system_package_manager_debian (move (*osr)));
        }
      }
    }

    if (r == nullptr)
    {
      if (!name.empty ())
        fail << "unsupported package manager '" << name << "' for host "
             << host;
    }

    return r;
  }
}
