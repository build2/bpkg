// file      : bpkg/package.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package>

#include <cassert>
#include <stdexcept> // invalid_argument

using namespace std;

namespace bpkg
{
  // repository
  //
  repository::_id_type repository::
  _id () const
  {
    return _id_type {location.canonical_name (), location.string ()};
  }

  void repository::
  _id (_id_type&& l)
  {
    location = repository_location (move (l.location));
    assert (location.canonical_name () == l.name);
  }

  // package_version_id
  //
  bool
  operator< (const package_version_id& x, const package_version_id& y)
  {
    int r (x.name.compare (y.name));

    if (r != 0)
      return r < 0;

    if (x.epoch != y.epoch)
      return x.epoch < y.epoch;

    r = x.upstream.compare (y.upstream);

    if (r != 0)
      return r < 0;

    return x.revision < y.revision;
  }

  // available_package
  //
  available_package::_id_type available_package::
  _id () const
  {
    return _id_type {package_version_id (name, version), version.upstream ()};
  }

  void available_package::
  _id (_id_type&& v)
  {
    name = move (v.data.name);
    version = version_type (v.data.epoch,
                            move (v.version_original_upstream),
                            v.data.revision);
    assert (version.canonical_upstream () == v.data.upstream);
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
