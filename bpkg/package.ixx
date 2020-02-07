// file      : bpkg/package.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace bpkg
{
  // available_package_id
  //
  inline available_package_id::
  available_package_id (package_name n, const bpkg::version& v)
      : name (move (n)),
        version (v)
  {
  }
}
