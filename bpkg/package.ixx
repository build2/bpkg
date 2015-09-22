// file      : bpkg/package.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace bpkg
{
  // package_version_id
  //
  inline package_version_id::
  package_version_id (string n, const version& v)
      : name (move (n)),
        epoch (v.epoch ()),
        upstream (v.canonical_upstream ()),
        revision (v.revision ())
  {
  }
}
