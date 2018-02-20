// file      : bpkg/package.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>

#include <algorithm> // find_if()

#include <bpkg/database.hxx>

using namespace std;

namespace bpkg
{
  const version wildcard_version (0, "0", nullopt, 0);

  // available_package_id
  //
  bool
  operator< (const available_package_id& x, const available_package_id& y)
  {
    int r (x.name.compare (y.name));
    return r != 0 ? r < 0 : x.version < y.version;
  }

  // available_package
  //
  // Check if the package is available from the specified repository, its
  // prerequisite repositories, or one of their complements, recursively.
  // Return the first repository that contains the package or NULL if none
  // are.
  //
  // Note that we can end up with a repository dependency cycle since the
  // root repository can be the default complement for git repositories (see
  // rep_fetch() implementation for details). Thus we need to make sure that
  // the repository is not in the dependency chain yet.
  //
  using repositories = vector<reference_wrapper<const shared_ptr<repository>>>;

  static shared_ptr<repository>
  find (const shared_ptr<repository>& r,
        const shared_ptr<available_package>& ap,
        repositories& chain)
  {
    auto pr = [&r] (const shared_ptr<repository>& i) -> bool {return i == r;};
    auto i (find_if (chain.begin (), chain.end (), pr));

    if (i != chain.end ())
      return nullptr;

    bool prereq (chain.empty ()); // Check prerequisites in top-level only.
    chain.emplace_back (r);

    unique_ptr<repositories, void (*)(repositories*)> deleter (
      &chain, [] (repositories* r) {r->pop_back ();});

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
        if (shared_ptr<repository> r = find (cr.load (), ap, chain))
          return r;
      }

      if (prereq)
      {
        for (const lazy_weak_ptr<repository>& pr: ps)
        {
          if (shared_ptr<repository> r = find (pr.load (), ap, chain))
            return r;
        }
      }
    }

    return nullptr;
  }

  static inline shared_ptr<repository>
  find (const shared_ptr<repository>& r,
        const shared_ptr<available_package>& ap)
  {
    repositories chain;
    return find (r, ap, chain);
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

  // selected_package
  //
  string selected_package::
  version_string () const
  {
    return version != wildcard_version ? version.string () : "*";
  }

  ostream&
  operator<< (ostream& os, const selected_package& p)
  {
    os << (p.system () ? "sys:" : "") << p.name << "/" << p.version_string ();
    return os;
  }

  // state
  //
  string
  to_string (package_state s)
  {
    switch (s)
    {
    case package_state::transient:  return "transient";
    case package_state::broken:     return "broken";
    case package_state::fetched:    return "fetched";
    case package_state::unpacked:   return "unpacked";
    case package_state::configured: return "configured";
    }

    return string (); // Should never reach.
  }

  package_state
  to_package_state (const string& s)
  {
         if (s == "transient")  return package_state::transient;
    else if (s == "broken")     return package_state::broken;
    else if (s == "fetched")    return package_state::fetched;
    else if (s == "unpacked")   return package_state::unpacked;
    else if (s == "configured") return package_state::configured;
    else throw invalid_argument ("invalid package state '" + s + "'");
  }

  // substate
  //
  string
  to_string (package_substate s)
  {
    switch (s)
    {
    case package_substate::none:   return "none";
    case package_substate::system: return "system";
    }

    return string (); // Should never reach.
  }

  package_substate
  to_package_substate (const string& s)
  {
         if (s == "none")   return package_substate::none;
    else if (s == "system") return package_substate::system;
    else throw invalid_argument ("invalid package substate '" + s + "'");
  }

  // certificate
  //
  ostream&
  operator<< (ostream& os, const certificate& c)
  {
    using butl::operator<<;

    if (c.dummy ())
      os << c.name << " (dummy)";
    else
      os << c.name << ", \"" << c.organization << "\" <" << c.email << ">, "
         << c.start_date << " - " << c.end_date << ", " << c.fingerprint;

    return os;
  }
}
