// file      : bpkg/pkg-verify.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-verify>

#include <fstream>

#include <butl/process>
#include <butl/fdstream>

#include <bpkg/manifest-parser>

#include <bpkg/types>
#include <bpkg/utility>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  package_manifest
  pkg_verify (const common_options& co, const path& af, bool diag)
  {
    // Figure out the package directory. Strip the top-level extension
    // and, as a special case, if the second-level extension is .tar,
    // strip that as well (e.g., .tar.bz2).
    //
    path pd (af.leaf ().base ());
    if (const char* e = pd.extension ())
    {
      if (e == string ("tar"))
        pd = pd.base ();
    }

    // Extract the manifest.
    //
    path mf (pd / path ("manifest"));

    cstrings args {co.tar ().string ().c_str ()};

    // Add extra options.
    //
    for (const string& o: co.tar_option ())
      args.push_back (o.c_str ());

    // -O/--to-stdout -- extract to STDOUT.
    //
    args.push_back ("-O");

    args.push_back ("-xf");
    args.push_back (af.string ().c_str ());
    args.push_back (mf.string ().c_str ());
    args.push_back (nullptr);

    if (verb >= 2)
      print_process (args);

    try
    {
      // If diag is false, we need to make tar not print any diagnostics.
      // There doesn't seem to be an option to suppress this and the only
      // way is to redirect STDERR to something like /dev/null. To keep
      // things simple, we are going to redirect it to STDOUT, which we
      // in turn redirect to a pipe and use to parse the manifest data.
      // If things go badly for tar and it starts spitting errors instead
      // of the manifest, the manifest parser will fail. But that's ok
      // since we assume that the child error is always the reason for
      // the manifest parsing failure.
      //
      process pr (args.data (), 0, -1, (diag ? 2 : 1));

      try
      {
        ifdstream is (pr.in_ofd);
        is.exceptions (ifdstream::badbit | ifdstream::failbit);

        manifest_parser mp (is, mf.string ());
        package_manifest m (mp);
        is.close ();

        if (pr.wait ())
        {
          // Verify package archive/directory is <name>-<version>.
          //
          path ed (m.name + "-" + m.version.string ());

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

        // Child existed with an error, fall through.
      }
      // Ignore these exceptions if the child process exited with
      // an error status since that's the source of the failure.
      //
      catch (const manifest_parsing& e)
      {
        if (pr.wait ())
        {
          if (diag)
            error (e.name, e.line, e.column) << e.description <<
              info << "package archive " << af;

          throw failed ();
        }
      }
      catch (const ifdstream::failure&)
      {
        if (pr.wait ())
        {
          if (diag)
            error << "unable to extract " << mf << " from " << af;

          throw failed ();
        }
      }

      // We should only get here if the child exited with an error
      // status.
      //
      assert (!pr.wait ());

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
      error << "unable to execute " << args[0] << ": " << e.what ();

      if (e.child ())
        exit (1);

      throw failed ();
    }
  }

  package_manifest
  pkg_verify (const dir_path& d, bool diag)
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
      ifstream ifs;
      ifs.exceptions (ifstream::badbit | ifstream::failbit);
      ifs.open (mf.string ());

      manifest_parser mp (ifs, mf.string ());
      package_manifest m (mp);

      // Verify package directory is <name>-<version>.
      //
      dir_path ed (m.name + "-" + m.version.string ());

      if (d.leaf () != ed)
      {
        if (diag)
          error << "invalid package directory name '" << d.leaf () << "'" <<
            info << "expected from manifest '" << ed << "'";

        throw failed ();
      }

      return m;
    }
    catch (const manifest_parsing& e)
    {
      if (diag)
        error (e.name, e.line, e.column) << e.description;

      throw failed ();
    }
    catch (const ifstream::failure&)
    {
      if (diag)
        error << "unable to read from " << mf;

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

    level4 ([&]{trace << "archive: " << a;});

    // If we were asked to run silent, don't yap about the reason
    // why the package is invalid. Just return the error status.
    //
    try
    {
      package_manifest m (pkg_verify (o, a, !o.silent ()));

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
