// file      : bpkg/manifest-utility.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace bpkg
{
  // NOTE: the implementation relies on the fact that only pkg and dir
  // repositories are currently supported.
  //
  inline bool
  masked_repository_fragment (const repository_location& rl)
  {
    return masked_repository (rl);
  }
}
