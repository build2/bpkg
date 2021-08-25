// file      : bpkg/pkg-checkout.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_CHECKOUT_HXX
#define BPKG_PKG_CHECKOUT_HXX

#include <map>

#include <libbpkg/manifest.hxx>     // version
#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package
#include <bpkg/utility.hxx>

#include <bpkg/pkg-checkout-options.hxx>

namespace bpkg
{
  int
  pkg_checkout (const pkg_checkout_options&, cli::scanner& args);

  // Checked out repository fragments cache.
  //
  // Needs to be passed to pkg_checkout() calls.
  //
  class pkg_checkout_cache
  {
  public:
    // The options reference is assumed to be valid till the end of the cache
    // object lifetime.
    //
    pkg_checkout_cache (const common_options& o): options_ (o) {}

    // Restore the cached repositories in their permanent locations (move back
    // from the temporary directory, fixup, etc) and erase the entries.
    //
    // Note that the destructor will clear the cache but will ignore any
    // errors. To detect such errors, call clear() explicitly.
    //
    bool
    clear (bool ignore_errors = false);

    // Call clear() in the ignore errors mode and issue the "repository is now
    // broken" warning on failure.
    //
    ~pkg_checkout_cache ();

    // pkg_checkout () implementation details.
    //
  public:
    struct state
    {
      auto_rmdir rmt;         // The repository temporary directory.
      repository_location rl; // The repository location.

      // nullopt if the repository fragment checkout failed in the middle and
      // the repository cannot be restored in its permanent location (we will
      // call such entry incomplete). True if the repository directory was
      // fixed up.
      //
      optional<bool> fixedup;
    };

    using state_map = std::map<dir_path, state>;

    state_map map_;
    const common_options& options_;

    // Restore the repository in its permanent location and erase the cache
    // entry. On error issue diagnostics and return false in the ignore errors
    // mode and throw failed otherwise. Note that erasing an incomplete entry
    // is an error.
    //
    bool
    erase (state_map::iterator, bool ignore_errors = false);
  };

  // Note that for the following functions both package and repository
  // information configurations need to be passed.
  //

  // Check out the package from a version control-based repository into a
  // directory other than the configuration directory and commit the
  // transaction. Return the selected package object which may replace the
  // existing one.
  //
  shared_ptr<selected_package>
  pkg_checkout (pkg_checkout_cache&,
                const common_options&,
                database& pdb,
                database& rdb,
                transaction&,
                package_name,
                version,
                const dir_path& output_root,
                bool replace,
                bool purge,
                bool simulate);

  // Check out the package from a version control-based repository and commit
  // the transaction. Return the selected package object which may replace the
  // existing one.
  //
  shared_ptr<selected_package>
  pkg_checkout (pkg_checkout_cache&,
                const common_options&,
                database& pdb,
                database& rdb,
                transaction&,
                package_name,
                version,
                bool replace,
                bool simulate);
}

#endif // BPKG_PKG_CHECKOUT_HXX
