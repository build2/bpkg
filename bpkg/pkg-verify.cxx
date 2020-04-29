// file      : bpkg/pkg-verify.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-verify.hxx>

#include <iostream> // cout

#include <libbutl/manifest-parser.mxx>
#include <libbutl/manifest-serializer.mxx>

#include <bpkg/archive.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  package_manifest
  pkg_verify (const common_options& co,
              const path& af,
              bool iu,
              bool ev,
              bool cd,
              bool diag)
  try
  {
    dir_path pd (package_dir (af));
    path mf (pd / manifest_file);

    // If diag is false, we need to make tar not print any diagnostics. There
    // doesn't seem to be an option to suppress this and the only way is to
    // redirect stderr to something like /dev/null.
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
      package_manifest m (mp, iu, cd);
      is.close ();

      if (wait ())
      {
        // Verify package archive/directory is <name>-<version>.
        //
        dir_path ed (m.name.string () + "-" + m.version.string ());

        if (pd != ed)
        {
          if (diag)
            error << "package archive/directory name mismatch in " << af <<
              info << "extracted from archive '" << pd << "'" <<
              info << "expected from manifest '" << ed << "'";

          throw failed ();
        }

        // Expand the *-file manifest values, if requested.
        //
        if (ev)
        {
          m.load_files (
            [&pd, &co, &af, diag] (const string& n, const path& p)
            {
              path f (pd / p);
              string s (extract (co, af, f, diag));

              if (s.empty ())
              {
                if (diag)
                  error << n << " manifest value in package archive "
                        << af << " references empty file " << f;

                throw failed ();
              }

              return s;
            },
            iu);
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
  pkg_verify (const dir_path& d,
              bool iu,
              const function<package_manifest::translate_function>& tf,
              bool diag)
  {
    // Parse the manifest.
    //
    path mf (d / manifest_file);

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
      package_manifest m (mp, tf, iu);

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
      package_manifest m (pkg_verify (o,
                                      a,
                                      o.ignore_unknown (),
                                      o.deep () /* expand_values */,
                                      o.deep () /* complete_depends */,
                                      !o.silent ()));

      if (o.manifest ())
      {
        try
        {
          manifest_serializer s (cout, "stdout");
          m.serialize (s);
        }
        catch (const manifest_serialization& e)
        {
          fail << "unable to serialize manifest: " << e.description;
        }
        catch (const io_error&)
        {
          fail << "unable to write to stdout";
        }
      }
      else if (verb && !o.silent () && !o.no_result ())
        text << "valid package " << m.name << " " << m.version;

      return 0;
    }
    catch (const failed&)
    {
      return 1;
    }
  }
}
