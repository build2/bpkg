// file      : bpkg/system-package-manager-debian.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager-debian.hxx>

#include <bpkg/diagnostics.hxx>

namespace bpkg
{
  // Do we use apt or apt-get? From apt(8):
  //
  // "The apt(8) commandline is designed as an end-user tool and it may change
  //  behavior between versions. [...]
  //
  //  All features of apt(8) are available in dedicated APT tools like
  //  apt-get(8) and apt-cache(8) as well. [...] So you should prefer using
  //  these commands (potentially with some additional options enabled) in
  //  your scripts as they keep backward compatibility as much as possible."

  // @@ We actually need to fetch if some are not installed to get their
  //    versions. We can do it as part of the call, no? Keep track if
  //    already fetched.

  // @@ We may map multiple our packages to the same system package
  //    (e.g., openssl-devel) so probably should track the status of
  //    individual system packages. What if we "installed any version"
  //    first and then need to install specific?
}
