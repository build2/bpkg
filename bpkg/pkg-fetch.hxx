// file      : bpkg/pkg-fetch.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_FETCH_HXX
#define BPKG_PKG_FETCH_HXX

#include <libbpkg/manifest.hxx>     // version
#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package, fetch_cache
#include <bpkg/utility.hxx>

#include <bpkg/pkg-fetch-options.hxx>

namespace bpkg
{
  int
  pkg_fetch (const pkg_fetch_options&, cli::scanner& args);

  // Fetch the package as an archive file and commit the transaction if
  // keep_transaction_if_safe is false or keeping it is deemed unsafe (see
  // below). Return the selected package object which may replace the existing
  // one.
  //
  // Note that it is deemed safe to keep the transaction running if no
  // filesystem state changes that would need to be tracked in the database
  // have been made. This is normally the case when we just save the path of
  // an existing archive to the selected package (while using an external
  // archive or cached archive when src caching is enabled) and, if replacing,
  // don't purge the current archive. In this case, if the transaction is
  // aborted after the function call, the database and filesystem states stay
  // consistent for the selected package.
  //
  shared_ptr<selected_package>
  pkg_fetch (const common_options&,
             database&,
             transaction&,
             path archive,
             bool replace,
             bool purge,
             bool simulate,
             bool keep_transaction_if_safe);

  // Fetch the package from an archive-based repository and commit the
  // transaction if keep_transaction_if_safe is false or keeping it is deemed
  // unsafe. If the fetch cache is enabled it should be already open (and this
  // function never closes it), unless in the simulation mode. Return the
  // selected package object which may replace the existing one.
  //
  // Note that both package and repository information configurations need to
  // be passed.
  //
  // Also note that it should be called in session.
  //
  shared_ptr<selected_package>
  pkg_fetch (const common_options&,
             fetch_cache&,
             database& pdb,
             database& rdb,
             transaction&,
             package_name,
             version,
             bool replace,
             bool simulate,
             bool keep_transaction_if_safe);

  pkg_fetch_options
  merge_options (const default_options<pkg_fetch_options>&,
                 const pkg_fetch_options&);
}

#endif // BPKG_PKG_FETCH_HXX
