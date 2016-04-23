// file      : bpkg/satisfaction.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/satisfaction>

#include <bpkg/package-odb>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  bool
  satisfies (const version& v, const dependency_constraint& c)
  {
    assert (!c.empty ());

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
}
