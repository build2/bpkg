// file      : bpkg/package.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>

#include <algorithm> // find_if()

#include <bpkg/database.hxx>
#include <bpkg/checksum.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;

namespace bpkg
{
  const version wildcard_version (0, "0", nullopt, 0, 0);

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
        repositories& chain,
        bool prereq)
  {
    // Prerequisites are not searched through recursively.
    //
    assert (!prereq || chain.empty ());

    auto pr = [&r] (const shared_ptr<repository>& i) -> bool {return i == r;};
    auto i (find_if (chain.begin (), chain.end (), pr));

    if (i != chain.end ())
      return nullptr;

    chain.emplace_back (r);

    unique_ptr<repositories, void (*)(repositories*)> deleter (
      &chain, [] (repositories* r) {r->pop_back ();});

    const auto& ps (r->prerequisites);
    const auto& cs (r->complements);

    // @@ The same repository can be present in the location set multiple times
    //    with different fragment values. Given that we may traverse the same
    //    repository tree multiple times, which is inefficient but harmless.
    //    Let's leave it this way for now as it likely to be changed with
    //    adding support for repository fragment objects.
    //
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
        if (shared_ptr<repository> r = find (cr.load (), ap, chain, false))
          return r;
      }

      if (prereq)
      {
        for (const lazy_weak_ptr<repository>& pr: ps)
        {
          if (shared_ptr<repository> r = find (pr.load (), ap, chain, false))
            return r;
        }
      }
    }

    return nullptr;
  }

  shared_ptr<repository>
  filter (const shared_ptr<repository>& r,
          const shared_ptr<available_package>& ap,
          bool prereq)
  {
    repositories chain;
    return find (r, ap, chain, prereq);
  }

  vector<shared_ptr<available_package>>
  filter (const shared_ptr<repository>& r,
          result<available_package>&& apr,
          bool prereq)
  {
    vector<shared_ptr<available_package>> aps;

    for (shared_ptr<available_package> ap: pointer_result (apr))
    {
      if (filter (r, ap, prereq) != nullptr)
        aps.push_back (move (ap));
    }

    return aps;
  }

  pair<shared_ptr<available_package>, shared_ptr<repository>>
  filter_one (const shared_ptr<repository>& r,
              result<available_package>&& apr,
              bool prereq)
  {
    using result = pair<shared_ptr<available_package>, shared_ptr<repository>>;

    for (shared_ptr<available_package> ap: pointer_result (apr))
    {
      if (shared_ptr<repository> pr = filter (r, ap, prereq))
        return result (move (ap), move (pr));
    }

    return result ();
  }

  vector<pair<shared_ptr<available_package>, shared_ptr<repository>>>
  filter (const vector<shared_ptr<repository>>& rps,
          odb::result<available_package>&& apr,
          bool prereq)
  {
    vector<pair<shared_ptr<available_package>, shared_ptr<repository>>> aps;

    for (shared_ptr<available_package> ap: pointer_result (apr))
    {
      for (const shared_ptr<repository> r: rps)
      {
        shared_ptr<repository> ar (filter (r, ap, prereq));

        if (ar != nullptr)
        {
          aps.emplace_back (move (ap), move (ar));
          break;
        }
      }
    }

    return aps;
  }

  void
  check_any_available (const dir_path& c,
                       transaction& t,
                       const diag_record* dr)
  {
    database& db (t.database ());

    if (db.query_value<repository_count> () == 0)
    {
      diag_record d;
      (dr != nullptr ? *dr << info : d << fail)
        << "configuration " << c << " has no repositories" <<
        info << "use 'bpkg rep-add' to add a repository";
    }
    else if (db.query_value<available_package_count> () == 0)
    {
      diag_record d;
      (dr != nullptr ? *dr << info : d << fail)
        << "configuration " << c << " has no available packages" <<
        info << "use 'bpkg rep-fetch' to fetch available packages list";
    }
  }

  string
  package_string (const string& n, const version& v, bool system)
  {
    string vs (v.empty ()
               ? string ()
               : v == wildcard_version
                 ? "/*"
                 : '/' + v.string ());

    return system ? "sys:" + n + vs : n + vs;
  }

  // selected_package
  //
  string selected_package::
  version_string () const
  {
    return version != wildcard_version ? version.string () : "*";
  }

  optional<version>
  package_iteration (const common_options& o,
                     const dir_path& c,
                     transaction& t,
                     const dir_path& d,
                     const string& n,
                     const version& v,
                     bool check_external)
  {
    tracer trace ("package_iteration");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    if (check_external)
    {
      using query = query<package_repository>;

      query q (
        query::package::id.name == n &&
        compare_version_eq (query::package::id.version, v, true, false));

      for (const auto& pr: db.query<package_repository> (q))
      {
        const shared_ptr<repository>& r (pr.repository);

        if (r->location.directory_based ())
          fail << "external package " << n << '/' << v
               << " is already available from "
               << r->location.canonical_name ();
      }
    }

    shared_ptr<selected_package> p (db.find<selected_package> (n));

    if (p == nullptr || !p->src_root ||
        compare_version_ne (v, p->version, true, false))
      return nullopt;

    string mc (sha256 (o, d / manifest_file));

    // The selected package must not be "simulated" (see pkg-build for
    // details).
    //
    assert (p->manifest_checksum);

    bool changed (mc != *p->manifest_checksum);

    // If the manifest didn't changed but the selected package points to an
    // external source directory, then we also check if the directory have
    // moved.
    //
    if (!changed && p->external ())
    {
      dir_path src_root (p->effective_src_root (c));

      // We need to complete and normalize the source directory as it may
      // generally be completed against the configuration directory (unlikely
      // but possible), that can be relative and/or not normalized.
      //
      src_root.complete ().normalize ();

      changed = src_root != dir_path (d).complete ().normalize ();
    }

    return !changed
      ? p->version
      : version (v.epoch,
                 v.upstream,
                 v.release,
                 v.revision,
                 p->version.iteration + 1);
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
