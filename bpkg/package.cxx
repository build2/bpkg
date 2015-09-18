// file      : bpkg/package.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package>

#include <stdexcept> // invalid_argument

using namespace std;

namespace bpkg
{
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
