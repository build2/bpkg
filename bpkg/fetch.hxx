// file      : bpkg/fetch.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_FETCH_HXX
#define BPKG_FETCH_HXX

#include <ctime> // time_t

#include <libbpkg/manifest.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/common-options.hxx>

namespace bpkg
{
  // Repository type pkg (fetch-pkg.cxx).
  //

  // If HTTP proxy is specified via the --pkg-proxy option, then use it for
  // fetching manifests and archives from the remote pkg repository.
  //
  pkg_repository_manifests
  pkg_fetch_repositories (const dir_path&, bool ignore_unknown);

  pair<pkg_repository_manifests, string /* checksum */>
  pkg_fetch_repositories (const common_options&,
                          const repository_location&,
                          bool ignore_unknown);

  pkg_package_manifests
  pkg_fetch_packages (const dir_path&, bool ignore_unknown);

  pair<pkg_package_manifests, string /* checksum */>
  pkg_fetch_packages (const common_options&,
                      const repository_location&,
                      bool ignore_unknown);

  signature_manifest
  pkg_fetch_signature (const common_options&,
                       const repository_location&,
                       bool ignore_unknown);

  void
  pkg_fetch_archive (const common_options&,
                     const repository_location&,
                     const path& archive,
                     const path& dest);

  // Repository type git (fetch-git.cxx).
  //

  // Create a git repository in the specified directory and prepare it for
  // fetching from the specified repository location. Note that the repository
  // URL fragment is neither used nor validated.
  //
  void
  git_init (const common_options&,
            const repository_location&,
            const dir_path&);

  // Fetch a git repository in the specifid directory (previously created by
  // git_init() for the references obtained with the repository URL fragment
  // filters, returning commit ids these references resolve to in the earliest
  // to latest order. Update the remote repository URL, if changed. After
  // fetching the repository working tree state is unspecified (see
  // git_checkout()).
  //
  // Note that submodules are not fetched.
  //
  struct git_fragment
  {
    // User-friendly fragment name is either a ref (tags/v1.2.3, heads/master,
    // HEAD) or an abbreviated commit id (0123456789ab).
    //
    string      commit;
    std::time_t timestamp;
    string      friendly_name;
  };

  vector<git_fragment>
  git_fetch (const common_options&,
             const repository_location&,
             const dir_path&);

  // Checkout the specified commit previously fetched by git_fetch().
  //
  // Note that submodules may not be checked out.
  //
  void
  git_checkout (const common_options&,
                const dir_path&,
                const string& commit);

  // Fetch (if necessary) and checkout submodules, recursively, in a working
  // tree previously checked out by git_checkout(). Update the remote
  // repository URL, if changed.
  //
  void
  git_checkout_submodules (const common_options&,
                           const repository_location&,
                           const dir_path&);


  // Verify that the symlinks target paths in the working tree are valid,
  // relative, and none of them refer outside the repository directory.
  //
  void
  git_verify_symlinks (const common_options&, const dir_path&);

  // Fix up or revert the fixes (including in submodules, recursively) in a
  // working tree previously checked out by git_checkout() or
  // git_checkout_submodules(). Return true if any changes have been made to
  // the filesystem. On error issue diagnostics and return nullopt in the
  // ignore errors mode and throw failed otherwise.
  //
  // Noop on POSIX. On Windows it may replace git's filesystem-agnostic
  // symlinks with hardlinks for the file targets and junctions for the
  // directory targets. Note that it still makes sure the working tree is
  // being treated by git as "clean" despite the changes.
  //
  optional<bool>
  git_fixup_worktree (const common_options&,
                      const dir_path&,
                      bool revert,
                      bool ignore_errors = false);

  // Low-level fetch API (fetch.cxx).
  //

  // Start the process of fetching the specified URL. If out is empty, then
  // fetch to stdout. In this case also don't show any progress unless we are
  // running verbose. If user_agent is empty, then send the default (fetch
  // program specific) User-Agent header value. If the HTTP proxy URL is not
  // empty and the URL to fetch is HTTP(S), then fetch it via the specified
  // proxy server converting the https URL scheme to http (see the --pkg-proxy
  // option for details).
  //
  process
  start_fetch (const common_options&,
               const string& url,
               const path& out = {},
               const string& user_agent = {},
               const butl::url& proxy = {});

  // Similar to the above but can only be used for fetching HTTP(S) URL to a
  // file. Additionally return the HTTP status code, if the underlying fetch
  // program provides an easy way to retrieve it, and 0 otherwise.
  //
  pair<process, uint16_t>
  start_fetch_http (const common_options&,
                    const string& url,
                    const path& out,
                    const string& user_agent = {},
                    const butl::url& proxy = {});

  // As above but fetches HTTP(S) URL to stdout, which can be read by the
  // caller from the specified stream. On HTTP errors (e.g., 404) this stream
  // may contain the error description returned by the server and the process
  // may exit with 0 code.
  //
  // Fetch process stderr redirect mode.
  //
  enum class stderr_mode
  {
    // Don't redirect stderr.
    //
    pass,

    // If the underlying fetch program provides an easy way to retrieve the
    // HTTP status code, then redirect the fetch process stderr to a pipe, so
    // that depending on the returned status code the caller can either drop
    // or dump the fetch process diagnostics. Otherwise, may still redirect
    // stderr for some implementation-specific reasons (to prevent the
    // underlying fetch program from interacting with the user, etc). The
    // caller can detect whether stderr is redirected or not by checking
    // process::in_efd.
    //
    redirect,

    // As above but if stderr is redirected, minimize the amount of
    // diagnostics printed by the fetch program by only printing errors. That
    // allows the caller to read stdout and stderr streams sequentially in the
    // blocking mode by assuming that the diagnostics always fits into the
    // pipe buffer. If stderr is not redirected, then ignore this mode in
    // favor of the more informative diagnostics.
    //
    redirect_quiet
  };

  pair<process, uint16_t>
  start_fetch_http (const common_options&,
                    const string& url,
                    ifdstream& out,
                    fdstream_mode out_mode,
                    stderr_mode,
                    const string& user_agent = {},
                    const butl::url& proxy = {});
}

#endif // BPKG_FETCH_HXX
