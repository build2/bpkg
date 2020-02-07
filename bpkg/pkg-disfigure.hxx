// file      : bpkg/pkg-disfigure.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_DISFIGURE_HXX
#define BPKG_PKG_DISFIGURE_HXX

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package
#include <bpkg/utility.hxx>

#include <bpkg/pkg-disfigure-options.hxx>

namespace bpkg
{
  int
  pkg_disfigure (const pkg_disfigure_options&, cli::scanner& args);

  // Disfigure the package, update its state, and commit the
  // transaction. If the package state is broken, then this
  // is taken to mean it hasn't been successfully configured
  // and no clean prior to disfigure is necessary (or possible,
  // for that matter).
  //
  void
  pkg_disfigure (const dir_path& configuration,
                 const common_options&,
                 transaction&,
                 const shared_ptr<selected_package>&,
                 bool clean,
                 bool simulate);
}

#endif // BPKG_PKG_DISFIGURE_HXX
