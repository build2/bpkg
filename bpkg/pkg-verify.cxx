// file      : bpkg/pkg-verify.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-verify.hxx>

#include <iostream> // cout

#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <bpkg/archive.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/satisfaction.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  pkg_verify_result
  pkg_verify (const common_options& co,
              manifest_parser& p,
              bool it,
              const path& what,
              int diag_level)
  {
    manifest_name_value nv (p.next ());

    // Make sure this is the start and we support the version.
    //
    if (!nv.name.empty ())
      throw manifest_parsing (p.name (), nv.name_line, nv.name_column,
                              "start of package manifest expected");

    if (nv.value != "1")
      throw manifest_parsing (p.name (), nv.value_line, nv.value_column,
                              "unsupported format version");

    pkg_verify_result r;

    // For the depends name, parse the value and if it contains the build2 or
    // bpkg constraints, verify that they are satisfied, if requested.
    //
    // Note that if the semantics of the depends value changes we may be
    // unable to parse some of them before we get to build2 or bpkg and issue
    // the user-friendly diagnostics. So we are going to ignore such depends
    // values. But that means that if the user made a mistake in build2/bpkg
    // then we will skip them as well. This, however, is not a problem since
    // the pre-parsed result will then be re-parsed (e.g., by the
    // package_manifest() constructor) which will diagnose any mistakes.
    //
    for (nv = p.next (); !nv.empty (); nv = p.next ())
    {
      if (nv.name == "depends")
      try
      {
        // Note that we don't have the dependent package name here (unless we
        // bother to retrieve it from the manifest in advance). This may cause
        // parsing of a dependency alternative to fail while verifying the
        // reflect clause (see dependency_alternative for details). That is,
        // however, OK since we don't expect any clauses for the build2 and
        // bpkg constraints and we just ignore failures for other depends
        // values (see above).
        //
        dependency_alternatives das (nv.value, package_name ());

        if (das.buildtime)
        {
          for (dependency_alternative& da: das)
          {
            for (dependency& d: da)
            {
              const package_name& dn (d.name);

              if (dn != "build2" && dn != "bpkg")
                continue;

              // Even if the toolchain build-time dependencies are requested
              // to be ignored let's make sure they are well-formed, i.e. they
              // are the only dependencies in the respective depends values.
              //
              if (da.size () != 1)
              {
                if (diag_level != 0)
                  error (p.name (), nv.value_line, nv.value_column)
                    << "multiple names in " << dn << " dependency";

                throw failed ();
              }

              if (das.size () != 1)
              {
                if (diag_level != 0)
                  error (p.name (), nv.value_line, nv.value_column)
                    << "alternatives in " << dn << " dependency";

                throw failed ();
              }

              if (dn == "build2")
              {
                if (!it && d.constraint && !satisfy_build2 (co, d))
                {
                  if (diag_level != 0)
                  {
                    diag_record dr (error);
                    dr << "unable to satisfy constraint (" << d << ")";

                    if (!what.empty ())
                      dr << " for package " << what;

                    dr << info << "available build2 version is "
                       << build2_version;
                  }

                  throw failed ();
                }

                r.build2_dependency = move (d);
              }
              else
              {
                if (!it && d.constraint && !satisfy_bpkg (co, d))
                {
                  if (diag_level != 0)
                  {
                    diag_record dr (error);
                    dr << "unable to satisfy constraint (" << d << ")";

                    if (!what.empty ())
                      dr << " for package " << what;

                    dr << info << "available bpkg version is "
                       << bpkg_version;
                  }

                  throw failed ();
                }

                r.bpkg_dependency = move (d);
              }
            }
          }
        }
      }
      catch (const manifest_parsing&) {} // Ignore

      r.push_back (move (nv));
    }

    // Make sure this is the end.
    //
    nv = p.next ();
    if (!nv.empty ())
      throw manifest_parsing (p.name (), nv.name_line, nv.name_column,
                              "single package manifest expected");

    return r;
  }

  package_manifest
  pkg_verify (const common_options& co,
              const path& af,
              bool iu,
              bool it,
              bool ev,
              bool lb,
              bool cd,
              int diag_level)
  try
  {
    dir_path pd (package_dir (af));
    path mf (pd / manifest_file);

    // If the diag level is less than 2, we need to make tar not print any
    // diagnostics. There doesn't seem to be an option to suppress this and
    // the only way is to redirect stderr to something like /dev/null.
    //
    // If things go badly for tar and it starts spitting errors instead of the
    // manifest, the manifest parser will fail. But that's ok since we assume
    // that the child error is always the reason for the manifest parsing
    // failure.
    //
    pair<process, process> pr (start_extract (co, af, mf, diag_level == 2));

    auto wait = [&pr] () {return pr.second.wait () && pr.first.wait ();};

    try
    {
      ifdstream is (move (pr.second.in_ofd), fdstream_mode::skip);
      manifest_parser mp (is, mf.string ());

      package_manifest m (mp.name (),
                          pkg_verify (co, mp, it, af, diag_level),
                          iu,
                          cd);

      is.close ();

      if (wait ())
      {
        // Verify package archive/directory is <name>-<version>.
        //
        dir_path ed (m.name.string () + '-' + m.version.string ());

        if (pd != ed)
        {
          if (diag_level != 0)
            error << "package archive/directory name mismatch in " << af <<
              info << "extracted from archive '" << pd << "'" <<
              info << "expected from manifest '" << ed << "'";

          throw failed ();
        }

        // If requested, expand file-referencing package manifest values.
        //
        if (ev || lb)
        {
          m.load_files (
            [ev, &pd, &co, &af, diag_level]
            (const string& n, const path& p) -> optional<string>
            {
              bool bf (n == "build-file");

              // Always expand the build-file values.
              //
              if (ev || bf)
              {
                path f (pd / p);
                string s (extract (co, af, f, diag_level != 0));

                if (s.empty () && !bf)
                {
                  if (diag_level != 0)
                    error << n << " manifest value in package archive "
                          << af << " references empty file " << f;

                  throw failed ();
                }

                return s;
              }
              else
                return nullopt;
            },
            iu);
        }

        // Load the bootstrap, root, and config/*.build buildfiles into the
        // respective *-build values, if requested and are not already
        // specified in the manifest.
        //
        // Note that we don't verify that the files are not empty.
        //
        if (lb)
        {
          paths ps (archive_contents (co, af, diag_level != 0));

          auto contains = [&ps] (const path& p)
          {
            return find (ps.begin (), ps.end (), p) != ps.end ();
          };

          auto extract_buildfiles = [&m, &co, &af, &ps, diag_level, &contains]
                                    (const path& b,
                                     const path& r,
                                     const dir_path& c,
                                     const string& ext)
          {
            if (!m.bootstrap_build)
              m.bootstrap_build = extract (co, af, b, diag_level != 0);

            if (!m.root_build && contains (r))
              m.root_build = extract (co, af, r, diag_level != 0);

            // Extract build/config/*.build files.
            //
            if (m.root_build)
            {
              vector<buildfile>& bs (m.buildfiles);
              size_t n (bs.size ());

              for (const path& ap: ps)
              {
                if (!ap.to_directory () && ap.sub (c))
                {
                  path p (ap.leaf (c));
                  const char* e (p.extension_cstring ());

                  // Only consider immediate sub-entries of the config/
                  // subdirectory.
                  //
                  if (e != nullptr && ext == e && p.simple ())
                  {
                    path f (c.leaf () / p.base ()); // Relative to build/.

                    if (find_if (bs.begin (), bs.end (),
                                 [&f] (const auto& v) {return v.path == f;}) ==
                        bs.end ())
                    {
                      bs.emplace_back (move (f),
                                       extract (co, af, ap, diag_level != 0));
                    }
                  }
                }
              }

              // To produce a stable result sort the appended *-build values.
              //
              if (bs.size () != n)
              {
                sort (bs.begin () + n, bs.end (),
                      [] (const auto& x, const auto& y)
                      {
                        return x.path < y.path;
                      });
              }
            }
          };

          // Set the manifest's alt_naming flag to the deduced value if absent
          // and verify that it matches otherwise.
          //
          auto alt_naming = [&m, diag_level, &af] (bool v)
          {
            if (!m.alt_naming)
            {
              m.alt_naming = v;
            }
            else if (*m.alt_naming != v)
            {
              if (diag_level != 0)
                error << "buildfile naming scheme mismatch between manifest "
                      << "and package archive " << af;

              throw failed ();
            }
          };

          // Check the alternative bootstrap file first since it is more
          // specific.
          //
          path bf;
          if (contains (bf = pd / alt_bootstrap_file))
          {
            alt_naming (true);

            extract_buildfiles (bf,
                                pd / alt_root_file,
                                pd / alt_config_dir,
                                alt_build_ext);
          }
          else if (contains (bf = pd / std_bootstrap_file))
          {
            alt_naming (false);

            extract_buildfiles (bf,
                                pd / std_root_file,
                                pd / std_config_dir,
                                std_build_ext);
          }
          else
          {
            if (diag_level != 0)
              error << "unable to find bootstrap.build file in package "
                    << "archive " << af;

            throw failed ();
          }
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
        if (diag_level != 0)
          error (e.name, e.line, e.column) << e.description <<
            info << "package archive " << af;

        throw failed ();
      }
    }
    catch (const io_error&)
    {
      if (wait ())
      {
        if (diag_level != 0)
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
    if (diag_level == 2)
      error << af << " does not appear to be a bpkg package";

    throw not_package ();
  }
  catch (const process_error& e)
  {
    // Note: this is not an "invalid package" case, so no diag check.
    //
    fail << "unable to extract manifest file from " << af << ": " << e
         << endf;
  }

  package_manifest
  pkg_verify (const common_options& co,
              const dir_path& d,
              bool iu,
              bool it,
              bool lb,
              const function<package_manifest::translate_function>& tf,
              int diag_level)
  {
    // Parse the manifest.
    //
    path mf (d / manifest_file);

    if (!exists (mf))
    {
      if (diag_level == 2)
        error << "no manifest file in package directory " << d;

      throw not_package ();
    }

    try
    {
      ifdstream ifs (mf);
      manifest_parser mp (ifs, mf.string ());

      package_manifest m (mp.name (),
                          pkg_verify (co, mp, it, d, diag_level),
                          tf,
                          iu);

      // Load the bootstrap, root, and config/*.build buildfiles into the
      // respective *-build values, if requested and if they are not already
      // specified in the manifest. But first expand the build-file manifest
      // values into the respective *-build values.
      //
      // Note that we don't verify that the files are not empty.
      //
      if (lb)
      {
        m.load_files (
          [&d, &mf, diag_level]
          (const string& n, const path& p) -> optional<string>
          {
            // Only expand the build-file values.
            //
            if (n == "build-file")
            {
              path f (d / p);

              try
              {
                ifdstream is (f);
                return is.read_text ();
              }
              catch (const io_error& e)
              {
                if (diag_level != 0)
                  error << "unable to read from " << f << " referenced by "
                        << n << " manifest value in " << mf << ": " << e;

                throw failed ();
              }
            }
            else
              return nullopt;
          },
          iu);

        try
        {
          load_package_buildfiles (m, d);
        }
        catch (const runtime_error& e)
        {
          if (diag_level != 0)
            error << e;

          throw failed ();
        }
      }

      // We used to verify package directory is <name>-<version> but it is
      // not clear why we should enforce it in this case (i.e., the user
      // provides us with a package directory).
      //
      // dir_path ed (m.name + '-' + m.version.string ());
      //
      // if (d.leaf () != ed)
      // {
      //   if (diag_level != 0)
      //     error << "invalid package directory name '" << d.leaf () << "'" <<
      //       info << "expected from manifest '" << ed << "'";
      //
      //   throw failed ();
      // }

      return m;
    }
    catch (const manifest_parsing& e)
    {
      if (diag_level != 0)
        error (e.name, e.line, e.column) << e.description;

      throw failed ();
    }
    catch (const io_error& e)
    {
      if (diag_level != 0)
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
        pkg_verify (o,
                    a,
                    o.ignore_unknown (),
                    o.ignore_unknown () /* ignore_toolchain */,
                    o.deep () /* expand_values */,
                    o.deep () /* load_buildfiles */,
                    o.deep () /* complete_values */,
                    o.silent () ? 0 : 2));

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
    catch (const failed& e)
    {
      return e.code;
    }
  }
}
