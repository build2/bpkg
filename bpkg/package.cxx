// file      : bpkg/package.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package>
#include <bpkg/package-odb>

#include <stdexcept> // invalid_argument

#include <bpkg/database>

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

  // available_package
  //

  // Check if the package is available from the specified repository,
  // its prerequisite repositories, or one of their complements,
  // recursively. Return the first repository that contains the
  // package or NULL if none are.
  //
  static shared_ptr<repository>
  find (const shared_ptr<repository>& r,
        const shared_ptr<available_package>& ap,
        bool prereq = true)
  {
    const auto& ps (r->prerequisites);
    const auto& cs (r->complements);

    for (const package_location& pl: ap->locations)
    {
      const lazy_shared_ptr<repository>& lr (pl.repository);

      // First check the repository itself.
      //
      if (lr.object_id () == r->name)
        return r;

      // Then check all the complements and prerequisites without
      // loading them.
      //
      if (cs.find (lr) != cs.end () || (prereq && ps.find (lr) != ps.end ()))
        return lr.load ();

      // Finally, load the complements and prerequisites and check them
      // recursively.
      //
      for (const lazy_shared_ptr<repository>& cr: cs)
      {
        // Should we consider prerequisites of our complements as our
        // prerequisites? I'd say not.
        //
        if (shared_ptr<repository> r = find (cr.load (), ap, false))
          return r;
      }

      if (prereq)
      {
        for (const lazy_weak_ptr<repository>& pr: ps)
        {
          if (shared_ptr<repository> r = find (pr.load (), ap, false))
            return r;
        }
      }
    }

    return nullptr;
  }

  vector<shared_ptr<available_package>>
  filter (const shared_ptr<repository>& r, result<available_package>&& apr)
  {
    vector<shared_ptr<available_package>> aps;

    for (shared_ptr<available_package> ap: pointer_result (apr))
    {
      if (find (r, ap) != nullptr)
        aps.push_back (move (ap));
    }

    return aps;
  }

  pair<shared_ptr<available_package>, shared_ptr<repository>>
  filter_one (const shared_ptr<repository>& r, result<available_package>&& apr)
  {
    using result = pair<shared_ptr<available_package>, shared_ptr<repository>>;

    for (shared_ptr<available_package> ap: pointer_result (apr))
    {
      if (shared_ptr<repository> pr = find (r, ap))
        return result (move (ap), move (pr));
    }

    return result ();
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
  to_state (const string& s)
  {
         if (s == "broken")     return state::broken;
    else if (s == "fetched")    return state::fetched;
    else if (s == "unpacked")   return state::unpacked;
    else if (s == "configured") return state::configured;
    else throw invalid_argument ("invalid package state '" + s + "'");
  }
}
