// file      : bpkg/package.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>

#include <bpkg/database.hxx>
#include <bpkg/checksum.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;

namespace bpkg
{
  const version wildcard_version (0, "0", nullopt, nullopt, 0);

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
  odb::result<available_package>
  query_available (database& db,
                   const package_name& name,
                   const optional<version_constraint>& c,
                   bool order)
  {
    using query = query<available_package>;

    query q (query::id.name == name);
    const auto& vm (query::id.version);

    // If there is a constraint, then translate it to the query. Otherwise,
    // get the latest version or stub versions if present.
    //
    if (c)
    {
      assert (c->complete ());

      // If the revision is not explicitly specified, then compare ignoring the
      // revision. The idea is that when the user runs 'bpkg build libfoo/1'
      // and there is 1+1 available, it should just work. The user shouldn't
      // have to spell the revision explicitly. Similarly, when we have
      // 'depends: libfoo == 1', then it would be strange if 1+1 did not
      // satisfy this constraint. The same for libfoo <= 1 -- 1+1 should
      // satisfy.
      //
      // Note that we always compare ignoring the iteration, as it can not be
      // specified in the manifest/command line. This way the latest iteration
      // will always be picked up.
      //
      query qs (compare_version_eq (vm,
                                    canonical_version (wildcard_version),
                                    false /* revision */,
                                    false /* iteration */));

      if (c->min_version &&
          c->max_version &&
          *c->min_version == *c->max_version)
      {
        const version& v (*c->min_version);

        q = q &&
            (compare_version_eq (vm,
                                 canonical_version (v),
                                 v.revision.has_value (),
                                 false /* iteration */) ||
             qs);
      }
      else
      {
        query qr (true);

        if (c->min_version)
        {
          const version& v (*c->min_version);
          canonical_version cv (v);
          bool rv (v.revision);

          if (c->min_open)
            qr = compare_version_gt (vm, cv, rv, false /* iteration */);
          else
            qr = compare_version_ge (vm, cv, rv, false /* iteration */);
        }

        if (c->max_version)
        {
          const version& v (*c->max_version);
          canonical_version cv (v);
          bool rv (v.revision);

          if (c->max_open)
            qr = qr && compare_version_lt (vm, cv, rv, false /* iteration */);
          else
            qr = qr && compare_version_le (vm, cv, rv, false /* iteration */);
        }

        q = q && (qr || qs);
      }
    }

    if (order)
      q += order_by_version_desc (vm);

    return db.query<available_package> (q);
  }

  // Check if the package is available from the specified repository fragment,
  // its prerequisite repositories, or one of their complements, recursively.
  // Return the first repository fragment that contains the package or NULL if
  // none are.
  //
  // Note that we can end up with a repository dependency cycle since the
  // root repository can be the default complement for dir and git
  // repositories (see rep_fetch() implementation for details). Thus we need
  // to make sure that the repository fragment is not in the dependency chain
  // yet.
  //
  using repository_fragments =
    vector<reference_wrapper<const shared_ptr<repository_fragment>>>;

  static shared_ptr<repository_fragment>
  find (const shared_ptr<repository_fragment>& rf,
        const shared_ptr<available_package>& ap,
        repository_fragments& chain,
        bool prereq)
  {
    // Prerequisites are not searched through recursively.
    //
    assert (!prereq || chain.empty ());

    auto i (find_if (chain.begin (), chain.end (),
                     [&rf] (const shared_ptr<repository_fragment>& i) -> bool
                     {
                       return i == rf;
                     }));

    if (i != chain.end ())
      return nullptr;

    chain.emplace_back (rf);

    unique_ptr<repository_fragments, void (*)(repository_fragments*)> deleter (
      &chain, [] (repository_fragments* rf) {rf->pop_back ();});

    const auto& cs (rf->complements);
    const auto& ps (rf->prerequisites);

    for (const package_location& pl: ap->locations)
    {
      const lazy_shared_ptr<repository_fragment>& lrf (pl.repository_fragment);

      // First check the repository itself.
      //
      if (lrf.object_id () == rf->name)
        return rf;

      // Then check all the complements and prerequisites repository fragments
      // without loading them. Though, we still need to load complement and
      // prerequisite repositories.
      //
      auto pr = [&lrf] (const repository::fragment_type& i)
      {
        return i.fragment == lrf;
      };

      for (const lazy_weak_ptr<repository>& r: cs)
      {
        const auto& frs (r.load ()->fragments);

        if (find_if (frs.begin (), frs.end (), pr) != frs.end ())
          return lrf.load ();
      }

      if (prereq)
      {
        for (const lazy_weak_ptr<repository>& r: ps)
        {
          const auto& frs (r.load ()->fragments);

          if (find_if (frs.begin (), frs.end (), pr) != frs.end ())
            return lrf.load ();
        }
      }

      // Finally, load the complements and prerequisites and check them
      // recursively.
      //
      for (const lazy_weak_ptr<repository>& cr: cs)
      {
        for (const auto& fr: cr.load ()->fragments)
        {
          // Should we consider prerequisites of our complements as our
          // prerequisites? I'd say not.
          //
          if (shared_ptr<repository_fragment> r =
              find (fr.fragment.load (), ap, chain, false))
            return r;
        }
      }

      if (prereq)
      {
        for (const lazy_weak_ptr<repository>& pr: ps)
        {
          for (const auto& fr: pr.load ()->fragments)
          {
            if (shared_ptr<repository_fragment> r =
                find (fr.fragment.load (), ap, chain, false))
              return r;
          }
        }
      }
    }

    return nullptr;
  }

  shared_ptr<repository_fragment>
  filter (const shared_ptr<repository_fragment>& r,
          const shared_ptr<available_package>& ap,
          bool prereq)
  {
    repository_fragments chain;
    return find (r, ap, chain, prereq);
  }

  vector<shared_ptr<available_package>>
  filter (const shared_ptr<repository_fragment>& r,
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

  pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>
  filter_one (const shared_ptr<repository_fragment>& r,
              result<available_package>&& apr,
              bool prereq)
  {
    using result = pair<shared_ptr<available_package>,
                        shared_ptr<repository_fragment>>;

    for (shared_ptr<available_package> ap: pointer_result (apr))
    {
      if (shared_ptr<repository_fragment> pr = filter (r, ap, prereq))
        return result (move (ap), move (pr));
    }

    return result ();
  }

  vector<pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>>
  filter (const vector<shared_ptr<repository_fragment>>& rps,
          odb::result<available_package>&& apr,
          bool prereq)
  {
    vector<pair<shared_ptr<available_package>,
                shared_ptr<repository_fragment>>> aps;

    for (shared_ptr<available_package> ap: pointer_result (apr))
    {
      for (const shared_ptr<repository_fragment>& r: rps)
      {
        if (shared_ptr<repository_fragment> rf = filter (r, ap, prereq))
        {
          aps.emplace_back (move (ap), move (rf));
          break;
        }
      }
    }

    return aps;
  }

  pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>
  filter_one (const vector<shared_ptr<repository_fragment>>& rps,
              odb::result<available_package>&& apr,
              bool prereq)
  {
    using result = pair<shared_ptr<available_package>,
                        shared_ptr<repository_fragment>>;

    for (shared_ptr<available_package> ap: pointer_result (apr))
    {
      for (const shared_ptr<repository_fragment>& r: rps)
      {
        if (shared_ptr<repository_fragment> rf = filter (r, ap, prereq))
          return result (move (ap), move (rf));
      }
    }

    return result ();
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
  package_string (const package_name& n, const version& v, bool system)
  {
    assert (!n.empty ());

    string vs (v.empty ()
               ? string ()
               : v == wildcard_version
                 ? "/*"
                 : '/' + v.string ());

    return system ? "sys:" + n.string () + vs : n.string () + vs;
  }

  string
  package_string (const package_name& name,
                  const optional<version_constraint>& constraint,
                  bool system)
  {
    // Fallback to the version type-based overload if the constraint is not
    // specified.
    //
    if (!constraint)
      return package_string (name, version (), system);

    // There are no scenarios where the version constrain is present but is
    // empty (both endpoints are nullopt).
    //
    assert (!constraint->empty ());

    // If the endpoint versions are equal then represent the constraint as the
    // "<name>/<version>" string rather than "<name> == <version>", using the
    // version type-based overload.
    //
    const optional<version>& min_ver (constraint->min_version);
    bool eq (min_ver == constraint->max_version);

    if (eq)
      return package_string (name, *min_ver, system);

    if (system)
      return package_string (name, version (), system) + "/...";

    // Quote the result as it contains the space character.
    //
    return "'" + name.string () + ' ' + constraint->string () + "'";
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
                     const package_name& n,
                     const version& v,
                     bool check_external)
  {
    tracer trace ("package_iteration");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    if (check_external)
    {
      using query = query<package_repository_fragment>;

      query q (
        query::package::id.name == n &&
        compare_version_eq (query::package::id.version,
                            canonical_version (v),
                            true /* revision */,
                            false /* iteration */));

      for (const auto& prf: db.query<package_repository_fragment> (q))
      {
        const shared_ptr<repository_fragment>& rf (prf.repository_fragment);

        if (rf->location.directory_based ())
          fail << "external package " << n << '/' << v
               << " is already available from "
               << rf->location.canonical_name ();
      }
    }

    shared_ptr<selected_package> p (db.find<selected_package> (n));

    if (p == nullptr || !p->src_root ||
        compare_version_ne (v,
                            p->version,
                            true /* revision */,
                            false /* iteration */))
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
      normalize (src_root, "package source");

      changed = src_root != normalize (d, "package source");
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
