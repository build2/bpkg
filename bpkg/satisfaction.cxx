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

    if (c.min_version)
      s = c.min_open ? v > *c.min_version : v >= *c.min_version;

    if (s && c.max_version)
      s = c.max_open ? v < *c.max_version : v <= *c.max_version;

    return s;
  }

  bool
  satisfies (const dependency_constraint& l, const dependency_constraint& r)
  {
    assert (!l.empty () && !r.empty ());

    bool s (false);

    if (l.min_version)
    {
      if (r.min_version)
      {
        if (l.min_open)
          // Doesn't matter if r is min_open or not.
          //
          s = *l.min_version >= *r.min_version;
        else
          s = r.min_open
            ? *l.min_version > *r.min_version
            : *l.min_version >= *r.min_version;
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
          if (l.max_open)
            // Doesn't matter if r is max_open or not.
            //
            s = *l.max_version <= *r.max_version;
          else
            s = r.max_open
              ? *l.max_version < *r.max_version
              : *l.max_version <= *r.max_version;
        }
        else
          // Doesn't matter what l.max_version is, so leave s to be true.
          ;
      }
      else
        s = !r.max_version;
    }

    return s;
  }
}
