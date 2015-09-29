// file      : bpkg/package.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package>

#include <stdexcept> // invalid_argument

using namespace std;

namespace bpkg
{
  // available_package_id
  //
  bool
  operator< (const available_package_id& x, const available_package_id& y)
  {
    int r (x.name.compare (y.name));

    if (r != 0)
      return r < 0;

    const auto& xv (x.version);
    const auto& yv (y.version);

    if (xv.epoch != yv.epoch)
      return xv.epoch < yv.epoch;

    r = xv.canonical_upstream.compare (yv.canonical_upstream);

    if (r != 0)
      return r < 0;

    return xv.revision < yv.revision;
  }

  // state
  //
  string
  to_string (state s)
  {
    switch (s)
    {
    case state::broken:     return "broken";
    case state::fetched:    return "fetched";
    case state::unpacked:   return "unpacked";
    case state::configured: return "configured";
    }

    return string (); // Should never reach.
  }

  state
  from_string (const string& s)
  {
         if (s == "broken")     return state::broken;
    else if (s == "fetched")    return state::fetched;
    else if (s == "unpacked")   return state::unpacked;
    else if (s == "configured") return state::configured;
    else                        throw invalid_argument (s);
  }
}
