// file      : bpkg/satisfaction.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/satisfaction.hxx>

#include <libbutl/process.mxx>

#include <bpkg/utility.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  bool
  satisfies (const version& v, const dependency_constraint& c)
  {
    assert (!c.empty ());

    if (v == wildcard_version)
      return true;

    bool s (true);

    // See notes in pkg-build:find_available() on ignoring revision in
    // comparison.
    //
    if (c.min_version)
    {
      int i (v.compare (*c.min_version, c.min_version->revision == 0));
      s = c.min_open ? i > 0 : i >= 0;
    }

    if (s && c.max_version)
    {
      int i (v.compare (*c.max_version, c.max_version->revision == 0));
      s = c.max_open ? i < 0 : i <= 0;
    }

    return s;
  }

  bool
  satisfies (const dependency_constraint& l, const dependency_constraint& r)
  {
    assert (!l.empty () && !r.empty ());

    // Note: the revision ignoring logic is still unclear/unimplemented. It
    // seems it will be specific to each case below.
    //
    bool s (false);

    if (l.min_version)
    {
      if (r.min_version)
      {
        int i (l.min_version->compare (*r.min_version, false));
        if (l.min_open)
          // Doesn't matter if r is min_open or not.
          //
          s = i >= 0;
        else
          s = r.min_open ? i > 0 : i >= 0;
      }
      else
        s = true; // Doesn't matter what l.min_version is.
    }
    else
      s = !r.min_version;

    if (s)
    {
      if (l.max_version)
      {
        if (r.max_version)
        {
          int i (l.max_version->compare (*r.max_version, false));
          if (l.max_open)
            // Doesn't matter if r is max_open or not.
            //
            s = i <= 0;
          else
            s = r.max_open ? i < 0 : i <= 0;
        }
        else
        {
          // Doesn't matter what l.max_version is, so leave s to be true.
        }
      }
      else
        s = !r.max_version;
    }

    return s;
  }

  static version build2_version;

  void
  satisfy_build2 (const common_options& co,
                  const string& pkg,
                  const dependency& d)
  {
    assert (d.name == "build2");

    // Extract, parse, and cache build2 version string.
    //
    if (build2_version.empty ())
    {
      const char* args[] = {name_b (co), "--version", nullptr};

      try
      {
        process_path pp (process::path_search (args[0], exec_dir));

        if (verb >= 3)
          print_process (args);

        process pr (pp, args, 0, -1); // Redirect STDOUT to pipe.

        string l;
        try
        {
          ifdstream is (move (pr.in_ofd), fdstream_mode::skip);
          getline (is, l);
          is.close ();

          if (pr.wait () && l.compare (0, 7, "build2 ") == 0)
          {
            try
            {
              build2_version = version (string (l, 7));
            }
            catch (const invalid_argument&) {} // Fall through.
          }

          // Fall through.
        }
        catch (const io_error&)
        {
          pr.wait ();
          // Fall through.
        }

        if (build2_version.empty ())
          fail << "unable to determine build2 version of " << args[0];
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e;

        if (e.child)
          exit (1);

        throw failed ();
      }
    }

    if (!satisfies (build2_version, d.constraint))
      fail << "unable to satisfy constraint (" << d << ") for package "
           << pkg <<
        info << "available build2 version is " << build2_version;
  }

  static version bpkg_version;

  void
  satisfy_bpkg (const common_options&, const string& pkg, const dependency& d)
  {
    assert (d.name == "bpkg");

    // Parse and cache bpkg version string.
    //
    if (bpkg_version.empty ())
      bpkg_version = version (BPKG_VERSION_STR);

    if (!satisfies (bpkg_version, d.constraint))
      fail << "unable to satisfy constraint (" << d << ") for package "
           << pkg <<
        info << "available bpkg version is " << bpkg_version;
  }
}
