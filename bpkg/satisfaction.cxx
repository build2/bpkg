// file      : bpkg/satisfaction.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/satisfaction.hxx>

#include <libbutl/process.mxx>

#include <bpkg/package-odb.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  bool
  satisfies (const version& v, const dependency_constraint& c)
  {
    assert (!c.empty () && c.complete ());

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
    assert (!l.empty () && l.complete () && !r.empty () && r.complete ());

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
  satisfy_build2 (const common_options& o,
                  const package_name& pkg,
                  const dependency& d)
  {
    assert (d.name == "build2");

    // Extract, parse, and cache build2 version string.
    //
    if (build2_version.empty ())
    {
      fdpipe pipe (open_pipe ());

      process pr (start_b (o,
                           pipe, 2 /* stderr */,
                           verb_b::quiet,
                           "--version"));

      // Shouldn't throw, unless something is severely damaged.
      //
      pipe.out.close ();

      string l;
      try
      {
        ifdstream is (move (pipe.in), fdstream_mode::skip);
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
        fail << "unable to determine build2 version of " << name_b (o);
    }

    if (!satisfies (build2_version, d.constraint))
      fail << "unable to satisfy constraint (" << d << ") for package "
           << pkg <<
        info << "available build2 version is " << build2_version;
  }

  static version bpkg_version;

  void
  satisfy_bpkg (const common_options&,
                const package_name& pkg,
                const dependency& d)
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
