// file      : bpkg/pkg-drop.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_DROP_HXX
#define BPKG_PKG_DROP_HXX

#include <set>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // database, selected_package
#include <bpkg/utility.hxx>

#include <bpkg/pkg-drop-options.hxx>

namespace bpkg
{
  int
  pkg_drop (const pkg_drop_options&, cli::scanner& args);

  // Examine the list of prerequisite packages and drop those that don't
  // have any dependents. Return the set of packages that were actually
  // dropped. Note that it should be called in session.
  //
  std::set<shared_ptr<selected_package>>
  pkg_drop (const dir_path& configuration,
            const common_options&,
            database&,
            const std::set<shared_ptr<selected_package>>&,
            bool prompt);
}

#endif // BPKG_PKG_DROP_HXX
