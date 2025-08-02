// file      : bpkg/pkg-checkout.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_CHECKOUT_HXX
#define BPKG_PKG_CHECKOUT_HXX

#include <map>

#include <libbpkg/manifest.hxx>     // version
#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction, selected_package, fetch_cache
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

    // Restore the cached repositories in their permanent locations (remove
    // the working trees, move back from the temporary directory, etc) and
    // erase the entries. On error issue diagnostics and return false if fail
    // is false and throw failed otherwise. Note that in both cases the
    // function will try to restore as many repositories as possible.
    //
    // Note that the destructor will clear the cache but will not fail on
    // errors. To detect such errors, call clear() explicitly.
    //
    bool
    clear (bool fail = true);

    // Call clear() in the non-failing mode and issue the "repository is now
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

      // False if the repository is spoiled and cannot be restored in its
      // permanent location (submodule checkout failed during fetch, etc).
      //
      bool valid;

      // True if the repository directory was fixed up. Only meaningful if
      // valid is true.
      //
      bool fixedup;

      // If specified (not NULL), then this is the state of a repository
      // located in the referenced fetch cache and prepared for checkout by
      // the respective load_*() function call. In this case, rmt is only used
      // to hold the repository path (not active). Is assumed to be valid till
      // the end of the cache object lifetime. Only meaningful if valid is
      // true.
      //
      bpkg::fetch_cache* fetch_cache;
    };

    using state_map = std::map<dir_path, state>;

    state_map map_;
    const common_options& options_;

    // Remove the repository working tree, return the repository to its
    // permanent location, and erase the cache entry. On error issue
    // diagnostics and return false if fail is false and throw failed
    // otherwise. Note that erasing an invalid entry is an error.
    //
    bool
    erase (state_map::iterator, bool fail = true);

    // Restore the repository working tree state (revert fixups, etc) and
    // erase the cache entry. Leave the repository in the temporary directory
    // and return the corresponding auto_rmdir object. On error issue
    // diagnostics and return an empty auto_rmdir object if fail is false and
    // throw failed otherwise. Note that releasing an invalid entry is an
    // error.
    //
    auto_rmdir
    release (state_map::iterator, bool fail = true);
  };

  // Note that for the following functions both package and repository
  // information configurations need to be passed. Also note that if the fetch
  // cache is enabled it should be pre-open.
  //

  // Check out the package from a version control-based repository into a
  // directory other than the configuration directory and commit the
  // transaction. Return the selected package object which may replace the
  // existing one.
  //
  shared_ptr<selected_package>
  pkg_checkout (fetch_cache&,
                pkg_checkout_cache&,
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
  pkg_checkout (fetch_cache&,
                pkg_checkout_cache&,
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
