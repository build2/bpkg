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

  auto system_package_manager_debian::
  pkg_status (const package_name& pn,
              const available_packages* aps,
              bool install,
              bool fetch) -> const vector<package_status>*
  {
    // First check the cache.
    //
    {
      auto i (status_cache_.find (pn));

      if (i != status_cache_.end ())
        return &i->second;

      if (aps == nullptr)
        return nullptr;
    }

    // Translate our package name to the system package names.
    //
    strings spns (system_package_names (*aps,
                                        os_release_.name_id,
                                        os_release_.version_id,
                                        os_release_.like_ids));

    // @@ TODO: fallback to our package name if empty (plus -dev if lib).
    // @@ TODO: split into packages/components

    vector<package_status> r;

    // First look for an already installed package.
    //


    // Next look for available versions if we are allowed to install.
    //
    // We only do this if we don't have a package already installed. This is
    // because while Debian generally supports installing multiple versions in
    // parallel, this is unlikely to be supported for the packages we are
    // interested in due to the underlying limitations.
    //
    // Specifically, the packages that we are primarily interested in are
    // libraries with headers and executables (tools). While Debian is able to
    // install multiple libraries in parallel, it normally can only install a
    // single set of headers, static libraries, pkg-config files, etc., (i.e.,
    // the -dev package) at a time due to them being installed into the same
    // location (e.g., /usr/include). The same holds for executables, which
    // are installed into the same location (e.g., /usr/bin).
    //
    // It is possible that a certain library has made arrangements for
    // multiple of its versions to co-exist. For example, hypothetically, our
    // libssl package could be mapped to both libssl1.1 libssl1.1-dev and
    // libssl3 libssl3-dev which could be installed at the same time (note
    // that it is not the case in reality; there is only libssl-dev). However,
    // in this case, we should probably also have two packages with separate
    // names (e.g., libssl and libssl3) that can also co-exist. An example of
    // this would be libQt5Core and libQt6Core. (Note that strictly speaking
    // there could be different degrees of co-existence: for the system
    // package manager it is sufficient for different versions not to clobber
    // each other's files while for us we may also need the ability to use
    // different versions in the base build).
    //
    // Note also that the above reasoning is quite C/C++-centric and it's
    // possible that multiple versions of libraries (or equivalent) for other
    // languages (e.g., Rust) can always co-exist. In this case we may need to
    // revise this decision.
    //
    if (install && r.empty ())
    {
      if (fetch && !fetched_)
      {
        // @@ TODO: apt-get update

        fetched_ = true;
      }
    }

    // Cache.
    //
    return &status_cache_.emplace (pn, move (r)).first->second;
  }

  bool system_package_manager_debian::
  pkg_install (const package_name&, const version&)
  {
    return false;
  }
}
