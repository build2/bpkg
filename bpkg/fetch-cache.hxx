// file      : bpkg/fetch-cache.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_FETCH_CACHE_HXX
#define BPKG_FETCH_CACHE_HXX

#include <odb/sqlite/database.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/diagnostics.hxx>
#include <bpkg/common-options.hxx>

namespace bpkg
{
  // The local fetch cache is a singleton that is described by a bunch of
  // static variables (not exposed). The class itself serves as a RAII lock --
  // while an instance is alive, we have the cache database locked and open.
  //
  // The cache by default resides in ~/.build2/cache/. The cache database
  // file is called fetch-cache.sqlite3. The cache data is stored in the
  // following subdirectories next to it:
  //
  //   pkg/  -- archive repositories metadata and package archives
  //   git/  -- git repositories in the fetched state
  //   src/  -- package source directories unpacked from archives or checked
  //            out (and distributed) from git repositories
  //
  class fetch_cache
  {
  public:
    // Return true if the fetch caching is not diabled (--no-fetch-cache).
    //
    static bool
    enabled (const common_options&);

    // Return true if we are in the offline mode (--offline).
    //
    // Note that we must respect it even if caching is disabled: while we
    // don't allow specifying --offline with --no-fetch-cache, caching can
    // also be disabled via BPKG_FETCH_CACHE=0.
    //
    static bool
    offline (const common_options&);

    // Return true if fetch caching is enabled and sharing of source
    // directories for dependencies is not disabled (--fetch-cache=no-src).
    //
    static bool
    cache_src (const common_options&);

    // Return true if fetch caching is enabled and caching of repository
    // authentication answers is not disabled (--fetch-cache=no-trust).
    //
    static bool
    cache_trust (const common_options&);

    // Lock and open the fetch cache.
    //
    // Issue diagnotics and throw failed if anything goes wrong. Issue
    // progress indication if waiting for the cache to become unlocked.
    //
    fetch_cache (const common_options&, tracer&);

  private:
    odb::sqlite::database db_;
  };
}

#endif // BPKG_FETCH_CACHE_HXX
