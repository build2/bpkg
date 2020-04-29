// file      : bpkg/fetch-pkg.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/fetch.hxx>

#include <sstream>

#include <libbutl/filesystem.mxx>      // cpfile ()
#include <libbutl/manifest-parser.mxx>

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
      string sha256sum (sha256 (s.c_str (), s.size ()));

      istringstream ts (s); // Text mode.

      manifest_parser mp (ts, url);
      M m (mp, ignore_unknown);

      if (pr.wait ())
        return make_pair (move (m), move (sha256sum));

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

    auto_rmfile arm (df);
    process pr (start_fetch (o,
                             u.string (),
                             df,
                             string () /* user_agent */,
                             o.pkg_proxy ()));

    if (!pr.wait ())
    {
      // While it is reasonable to assuming the child process issued
      // diagnostics, some may not mention the URL.
      //
      fail << "unable to fetch " << u <<
        info << "re-run with -v for more information";
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
      string sha256sum;
      if (o != nullptr)
        sha256sum = sha256 (*o, f); // Read file in the binary mode.

      ifdstream ifs (f);  // Open file in the text mode.

      manifest_parser mp (ifs, f.string ());
      return make_pair (M (mp, ignore_unknown), move (sha256sum));
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
    return fetch_manifest<pkg_repository_manifests> (
      nullptr, d / repositories_file, iu).first;
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

    return rl.remote ()
      ? fetch_manifest<pkg_repository_manifests> (o, u, iu)
      : fetch_manifest<pkg_repository_manifests> (&o, f, iu);
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
