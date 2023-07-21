// file      : bpkg/fetch-pkg.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch.hxx>

#include <sstream>

#include <libbutl/filesystem.hxx>      // cpfile ()
#include <libbutl/manifest-parser.hxx>

#include <bpkg/checksum.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  template <typename M>
  static pair<M, string/*checksum*/>
  fetch_manifest (const common_options& o,
                  const repository_url& u,
                  bool ignore_unknown)
  {
    string url (u.string ());
    process pr (start_fetch (o,
                             url,
                             path () /* out */,
                             string () /* user_agent */,
                             o.pkg_proxy ()));

    try
    {
      // Unfortunately we cannot read from the original source twice as we do
      // below for files. There doesn't seem to be anything better than reading
      // the entire file into memory and then streaming it twice, once to
      // calculate the checksum and the second time to actually parse. We need
      // to read the original stream in the binary mode for the checksum
      // calculation, then use the binary data to create the text stream for
      // the manifest parsing.
      //
      ifdstream is (move (pr.in_ofd), fdstream_mode::binary);
      stringstream bs (ios::in | ios::out | ios::binary);

      // Note that the eof check is important: if the stream is at eof, write
      // will fail.
      //
      if (is.peek () != ifdstream::traits_type::eof ())
        bs << is.rdbuf ();

      is.close ();

      string s (bs.str ());
      string cs (sha256sum (s.c_str (), s.size ()));

      istringstream ts (s); // Text mode.

      manifest_parser mp (ts, url);
      M m (mp, ignore_unknown);

      if (pr.wait ())
        return make_pair (move (m), move (cs));

      // Child existed with an error, fall through.
    }
    // Ignore these exceptions if the child process exited with
    // an error status since that's the source of the failure.
    //
    catch (const manifest_parsing& e)
    {
      if (pr.wait ())
        fail (e.name, e.line, e.column) << e.description;
    }
    catch (const io_error&)
    {
      if (pr.wait ())
        fail << "unable to read fetched " << url;
    }

    // We should only get here if the child exited with an error status.
    //
    assert (!pr.wait ());

    // While it is reasonable to assuming the child process issued
    // diagnostics, some may not mention the URL.
    //
    fail << "unable to fetch " << url <<
      info << "re-run with -v for more information" << endf;
  }

  static void
  fetch_file (const common_options& o,
              const repository_url& u,
              const path& df)
  {
    if (exists (df))
      fail << "file " << df << " already exists";

    // Currently we only expect fetching a package archive via the HTTP(S)
    // protocol.
    //
    switch (u.scheme)
    {
    case repository_protocol::git:
    case repository_protocol::ssh:
    case repository_protocol::file: assert (false);
    case repository_protocol::http:
    case repository_protocol::https: break;
    }

    auto_rmfile arm (df);

    // Note that a package file may not be present in the repository due to
    // outdated repository information. Thus, while fetching the file we also
    // try to retrieve the HTTP status code. If the HTTP status code is
    // retrieved and is 404 (not found) or the fetch program doesn't support
    // its retrieval and fails, then we also advise the user to re-fetch the
    // repositories.
    //
    pair<process, uint16_t> ps (
      start_fetch_http (o,
                        u.string (),
                        df,
                        string () /* user_agent */,
                        o.pkg_proxy ()));

    process& pr (ps.first);
    uint16_t sc (ps.second);

    // Fail if the fetch process didn't exit normally with 0 code or the HTTP
    // status code is retrieved and differs from 200.
    //
    // Note that the diagnostics may potentially look as follows:
    //
    // foo-1.0.0.tar.gz:
    // ###################################################### 100.0%
    // error: unable to fetch package https://example.org/1/foo-1.0.0.tar.gz
    //  info: repository metadata could be stale
    //  info: run 'bpkg rep-fetch' (or equivalent) to update
    //
    // It's a bit unfortunate that the 100% progress indicator can be shown
    // for a potential HTTP error and it doesn't seem that we can easily fix
    // that. Note, however, that this situation is not very common and
    // probably that's fine.
    //
    if (!pr.wait () || (sc != 0 && sc != 200))
    {
      // While it is reasonable to assuming the child process issued
      // diagnostics, some may not mention the URL.
      //
      diag_record dr (fail);
      dr << "unable to fetch package " << u;

      // Print the HTTP status code in the diagnostics on the request failure,
      // unless it cannot be retrieved or is 404. Note that the fetch program
      // may even exit successfully on such a failure (see start_fetch_http()
      // for details) and issue no diagnostics at all.
      //
      if (sc != 0 && sc != 200 && sc != 404)
        dr << info << "HTTP status code " << sc;

      // If not found, advise the user to re-fetch the repositories. Note that
      // if the status code cannot be retrieved, we assume it could be 404 and
      // advise.
      //
      if (sc == 404 || sc == 0)
      {
        dr << info << "repository metadata could be stale" <<
              info << "run 'bpkg rep-fetch' (or equivalent) to update";
      }
      else if (verb < 2)
        dr << info << "re-run with -v for more information";
    }

    arm.cancel ();
  }

  static void
  fetch_file (const path& sf, const path& df)
  {
    try
    {
      cpfile (sf, df);
    }
    catch (const system_error& e)
    {
      fail << "unable to copy " << sf << " to " << df << ": " << e;
    }
  }

  // If o is nullptr, then don't calculate the checksum.
  //
  template <typename M>
  static pair<M, string/*checksum*/>
  fetch_manifest (const common_options* o,
                  const path& f,
                  bool ignore_unknown)
  {
    if (!exists (f))
      fail << "file " << f << " does not exist";

    try
    {
      // We can not use the same file stream for both calculating the checksum
      // and reading the manifest. The file should be opened in the binary
      // mode for the first operation and in the text mode for the second one.
      //
      string cs;
      if (o != nullptr)
        cs = sha256sum (*o, f); // Read file in the binary mode.

      ifdstream ifs (f);  // Open file in the text mode.

      manifest_parser mp (ifs, f.string ());
      return make_pair (M (mp, ignore_unknown), move (cs));
    }
    catch (const manifest_parsing& e)
    {
      fail (e.name, e.line, e.column) << e.description << endf;
    }
    catch (const io_error& e)
    {
      fail << "unable to read from " << f << ": " << e << endf;
    }
  }

  pkg_repository_manifests
  pkg_fetch_repositories (const dir_path& d, bool iu)
  {
    pkg_repository_manifests r (
      fetch_manifest<pkg_repository_manifests> (
        nullptr, d / repositories_file, iu).first);

    if (r.empty ())
      r.emplace_back (repository_manifest ()); // Add the base repository.

    return r;
  }

  pair<pkg_repository_manifests, string/*checksum*/>
  pkg_fetch_repositories (const common_options& o,
                          const repository_location& rl,
                          bool iu)
  {
    assert (rl.remote () || rl.absolute ());

    repository_url u (rl.url ());

    path& f (*u.path);
    f /= repositories_file;

    pair<pkg_repository_manifests, string> r (
      rl.remote ()
      ? fetch_manifest<pkg_repository_manifests> (o, u, iu)
      : fetch_manifest<pkg_repository_manifests> (&o, f, iu));

    if (r.first.empty ())
      r.first.emplace_back (repository_manifest ()); // Add the base repository.

    return r;
  }

  pkg_package_manifests
  pkg_fetch_packages (const dir_path& d, bool iu)
  {
    return fetch_manifest<pkg_package_manifests> (
      nullptr, d / packages_file, iu).first;
  }

  pair<pkg_package_manifests, string/*checksum*/>
  pkg_fetch_packages (const common_options& o,
                      const repository_location& rl,
                      bool iu)
  {
    assert (rl.remote () || rl.absolute ());

    repository_url u (rl.url ());

    path& f (*u.path);
    f /= packages_file;

    return rl.remote ()
      ? fetch_manifest<pkg_package_manifests> (o, u, iu)
      : fetch_manifest<pkg_package_manifests> (&o, f, iu);
  }

  signature_manifest
  pkg_fetch_signature (const common_options& o,
                       const repository_location& rl,
                       bool iu)
  {
    assert (rl.remote () || rl.absolute ());

    repository_url u (rl.url ());

    path& f (*u.path);
    f /= signature_file;

    return rl.remote ()
      ? fetch_manifest<signature_manifest> (o, u, iu).first
      : fetch_manifest<signature_manifest> (nullptr, f, iu).first;
  }

  void
  pkg_fetch_archive (const common_options& o,
                     const repository_location& rl,
                     const path& a,
                     const path& df)
  {
    assert (!a.empty () && a.relative ());
    assert (rl.remote () || rl.absolute ());

    repository_url u (rl.url ());

    path& sf (*u.path);
    sf /= a;

    auto bad_loc = [&u] () {fail << "invalid archive location " << u;};

    try
    {
      sf.normalize ();

      if (*sf.begin () == "..") // Can be the case for the remote location.
        bad_loc ();
    }
    catch (const invalid_path&)
    {
      bad_loc ();
    }

    if (rl.remote ())
      fetch_file (o, u, df);
    else
      fetch_file (sf, df);
  }
}
