// file      : bpkg/pkg-purge.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_PURGE_HXX
#define BPKG_PKG_PURGE_HXX

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package
#include <bpkg/utility.hxx>

#include <bpkg/pkg-purge-options.hxx>

namespace bpkg
{
  int
  pkg_purge (const pkg_purge_options&, cli::scanner& args);

  // Purge the package, remove it from the database, and commit the
  // transaction. If this fails, set the package state to broken.
  //
  void
  pkg_purge (database&,
             transaction&,
             const shared_ptr<selected_package>&,
             bool simulate);

  // Remove package's filesystem objects (the source directory and, if
  // the archive argument is true, the package archive). If this fails,
  // set the package state to broken, commit the transaction, and fail.
  //
  void
  pkg_purge_fs (database&,
                transaction&,
                const shared_ptr<selected_package>&,
                bool simulate,
                bool archive = true);
}

#endif // BPKG_PKG_PURGE_HXX
