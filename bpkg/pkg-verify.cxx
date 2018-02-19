// file      : bpkg/pkg-verify.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-verify.hxx>

#include <libbutl/process.mxx>
#include <libbutl/manifest-parser.mxx>

#include <bpkg/archive.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  package_manifest
  pkg_verify (const common_options& co, const path& af, bool iu, bool diag)
  try
  {
    dir_path pd (package_dir (af));
    path mf (pd / path ("manifest"));

    // If diag is false, we need to make tar not print any diagnostics. There
    // doesn't seem to be an option to suppress this and the only way is to
    // redirect STDERR to something like /dev/null.
    //
    // If things go badly for tar and it starts spitting errors instead of the
    // manifest, the manifest parser will fail. But that's ok since we assume
    // that the child error is always the reason for the manifest parsing
    // failure.
    //
    pair<process, process> pr (start_extract (co, af, mf, diag));

    auto wait = [&pr] () {return pr.second.wait () && pr.first.wait ();};

    try
    {
      ifdstream is (move (pr.second.in_ofd), fdstream_mode::skip);
      manifest_parser mp (is, mf.string ());
      package_manifest m (bpkg_package_manifest (mp, iu));
      is.close ();

      if (wait ())
      {
        // Verify package archive/directory is <name>-<version>.
        //
        dir_path ed (m.name + "-" + m.version.string ());

        if (pd != ed)
        {
          if (diag)
            error << "package archive/directory name mismatch in " << af <<
              info << "extracted from archive '" << pd << "'" <<
              info << "expected from manifest '" << ed << "'";

          throw failed ();
        }

        return m;
      }

      // Child exited with an error, fall through.
    }
    // Ignore these exceptions if the child process exited with
    // an error status since that's the source of the failure.
    //
    catch (const manifest_parsing& e)
    {
      if (wait ())
      {
        if (diag)
          error (e.name, e.line, e.column) << e.description <<
            info << "package archive " << af;

        throw failed ();
      }
    }
    catch (const io_error&)
    {
      if (wait ())
      {
        if (diag)
          error << "unable to extract " << mf << " from " << af;

        throw failed ();
      }
    }

    // We should only get here if the child exited with an error
    // status.
    //
    assert (!wait ());

    // While it is reasonable to assuming the child process issued
    // diagnostics, tar, specifically, doesn't mention the archive
    // name.
    //
    if (diag)
      error << af << " does not appear to be a bpkg package";

    throw failed ();
  }
  catch (const process_error& e)
  {
    // Note: this is not an "invalid package" case, so no diag check.
    //
    fail << "unable to extract manifest file from " << af << ": " << e
         << endf;
  }

  package_manifest
  pkg_verify (const dir_path& d, bool iu, bool diag)
  {
    // Parse the manifest.
    //
    path mf (d / path ("manifest"));

    if (!exists (mf))
    {
      if (diag)
        error << "no manifest file in package directory " << d;

      throw failed ();
    }

    try
    {
      ifdstream ifs (mf);
      manifest_parser mp (ifs, mf.string ());
      package_manifest m (bpkg_package_manifest (mp, iu));

      // We used to verify package directory is <name>-<version> but it is
      // not clear why we should enforce it in this case (i.e., the user
      // provides us with a package directory).
      //
      // dir_path ed (m.name + "-" + m.version.string ());
      //
      // if (d.leaf () != ed)
      // {
      //  if (diag)
      //    error << "invalid package directory name '" << d.leaf () << "'" <<
      //      info << "expected from manifest '" << ed << "'";
      //
      //  throw failed ();
      // }

      return m;
    }
    catch (const manifest_parsing& e)
    {
      if (diag)
        error (e.name, e.line, e.column) << e.description;

      throw failed ();
    }
    catch (const io_error& e)
    {
      if (diag)
        error << "unable to read from " << mf << ": " << e;

      throw failed ();
    }
  }

  int
  pkg_verify (const pkg_verify_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_verify");

    if (!args.more ())
      fail << "archive path argument expected" <<
        info << "run 'bpkg help pkg-verify' for more information";

    path a (args.next ());

    if (!exists (a))
      fail << "archive file '" << a << "' does not exist";

    l4 ([&]{trace << "archive: " << a;});

    // If we were asked to run silent, don't yap about the reason
    // why the package is invalid. Just return the error status.
    //
    try
    {
      package_manifest m (
        pkg_verify (o, a, o.ignore_unknown (), !o.silent ()));

      if (verb && !o.silent ())
        text << "valid package " << m.name << " " << m.version;

      return 0;
    }
    catch (const failed&)
    {
      return 1;
    }
  }
}
