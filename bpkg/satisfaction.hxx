// file      : bpkg/satisfaction.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SATISFACTION_HXX
#define BPKG_SATISFACTION_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>
#include <bpkg/common-options.hxx>

namespace bpkg
{
  // Note: all of the following functions expect the package dependency
  // constraints to be complete.

  // Return true if version satisfies the constraint.
  //
  bool
  satisfies (const version&, const dependency_constraint&);

  inline bool
  satisfies (const version& v, const optional<dependency_constraint>& c)
  {
    return !c || satisfies (v, *c);
  }

  // Return true if any version that satisfies l also satisfies r, or, in
  // other words, l is stricter than or equal to r. Or, in yet other words,
  // l is a subset of r.
  //
  bool
  satisfies (const dependency_constraint& l, const dependency_constraint& r);

  inline bool
  satisfies (const optional<dependency_constraint>& l,
             const optional<dependency_constraint>& r)
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
