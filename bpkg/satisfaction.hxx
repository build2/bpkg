// file      : bpkg/satisfaction.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SATISFACTION_HXX
#define BPKG_SATISFACTION_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>
#include <bpkg/common-options.hxx>

namespace bpkg
{
  // Note: all of the following functions expect the package version
  // constraints to be complete.

  // Return true if version satisfies the constraint.
  //
  bool
  satisfies (const version&, const version_constraint&);

  inline bool
  satisfies (const version& v, const optional<version_constraint>& c)
  {
    return !c || satisfies (v, *c);
  }

  // Return true if any version that satisfies l also satisfies r, or, in
  // other words, l is stricter than or equal to r. Or, in yet other words,
  // l is a subset of r.
  //
  bool
  satisfies (const version_constraint& l, const version_constraint& r);

  inline bool
  satisfies (const optional<version_constraint>& l,
             const optional<version_constraint>& r)
  {
    return l ? (!r || satisfies (*l, *r)) : !r;
  }

  // Special build-time dependencies.
  //
  void
  satisfy_build2 (const common_options&,
                  const package_name&,
                  const dependency&);

  void
  satisfy_bpkg (const common_options&, const package_name&, const dependency&);
}

#endif // BPKG_SATISFACTION_HXX
