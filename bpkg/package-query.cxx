// file      : bpkg/package-query.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package-query.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/rep-mask.hxx>
#include <bpkg/satisfaction.hxx>

using namespace std;

namespace bpkg
{
  // Search in the imaginary system repository.
  //
  vector<shared_ptr<available_package>> imaginary_stubs;

  shared_ptr<available_package>
  find_imaginary_stub (const package_name& name)
  {
    auto i (find_if (imaginary_stubs.begin (), imaginary_stubs.end (),
                     [&name] (const shared_ptr<available_package>& p)
                     {
                       return p->id.name == name;
                     }));

    return i != imaginary_stubs.end () ? *i : nullptr;
  }

  // Search in the existing packages registry.
  //
  vector<pair<reference_wrapper<database>,
              shared_ptr<available_package>>> existing_packages;

  pair<shared_ptr<available_package>, lazy_shared_ptr<repository_fragment>>
  find_existing (database& db,
                 const package_name& name,
                 const optional<version_constraint>& c)
  {
    pair<shared_ptr<available_package>,
         lazy_shared_ptr<repository_fragment>> r;

    for (const auto& p: existing_packages)
    {
      if (p.first == db             &&
          p.second->id.name == name &&
          (!c || satisfies (p.second->version, *c)))
      {
        r.first = p.second;
        r.second = lazy_shared_ptr<repository_fragment> (db, empty_string);
        break;
      }
    }

    return r;
  }

  // Search in real repositories.
  //
  linked_databases repo_configs;

  linked_databases
  dependent_repo_configs (database& db)
  {
    linked_databases r;
    for (database& ddb: db.dependent_configs ())
    {
      if (find (repo_configs.begin (), repo_configs.end (), ddb) !=
          repo_configs.end ())
        r.push_back (ddb);
    }

    return r;
  }

  odb::result<available_package>
  query_available (database& db,
                   const package_name& name,
                   const optional<version_constraint>& c,
                   bool order,
                   bool revision)
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
                                 revision || v.revision.has_value (),
                                 revision /* iteration */) ||
             qs);
      }
      else
      {
        query qr (true);

        if (c->min_version)
        {
          const version& v (*c->min_version);
          canonical_version cv (v);
          bool rv (revision || v.revision);

          if (c->min_open)
            qr = compare_version_gt (vm, cv, rv, revision /* iteration */);
          else
            qr = compare_version_ge (vm, cv, rv, revision /* iteration */);
        }

        if (c->max_version)
        {
          const version& v (*c->max_version);
          canonical_version cv (v);
          bool rv (revision || v.revision);

          if (c->max_open)
            qr = qr && compare_version_lt (vm, cv, rv, revision);
          else
            qr = qr && compare_version_le (vm, cv, rv, revision);
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

    if (find_if (chain.begin (), chain.end (),
                 [&rf] (const shared_ptr<repository_fragment>& i) -> bool
                 {
                   return i == rf;
                 }) != chain.end ())
      return nullptr;

    chain.emplace_back (rf);

    unique_ptr<repository_fragments, void (*)(repository_fragments*)> deleter (
      &chain, [] (repository_fragments* rf) {rf->pop_back ();});

    const auto& cs (rf->complements);
    const auto& ps (rf->prerequisites);

    for (const package_location& pl: ap->locations)
    {
      const lazy_shared_ptr<repository_fragment>& lrf (pl.repository_fragment);

      if (rep_masked_fragment (lrf))
        continue;

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
        if (!rep_masked (r))
        {
          const auto& frs (r.load ()->fragments);

          if (find_if (frs.begin (), frs.end (), pr) != frs.end ())
            return lrf.load ();
        }
      }

      if (prereq)
      {
        for (const lazy_weak_ptr<repository>& r: ps)
        {
          if (!rep_masked (r))
          {
            const auto& frs (r.load ()->fragments);

            if (find_if (frs.begin (), frs.end (), pr) != frs.end ())
              return lrf.load ();
          }
        }
      }
    }

    // Finally, load the complements and prerequisites and check them
    // recursively.
    //
    for (const lazy_weak_ptr<repository>& cr: cs)
    {
      if (!rep_masked (cr))
      {
        for (const auto& fr: cr.load ()->fragments)
        {
          // Should we consider prerequisites of our complements as our
          // prerequisites? I'd say not.
          //
          if (shared_ptr<repository_fragment> r =
              find (fr.fragment.load (), ap, chain, false /* prereq */))
            return r;
        }
      }
    }

    if (prereq)
    {
      for (const lazy_weak_ptr<repository>& pr: ps)
      {
        if (!rep_masked (pr))
        {
          for (const auto& fr: pr.load ()->fragments)
          {
            if (shared_ptr<repository_fragment> r =
                find (fr.fragment.load (), ap, chain, false /* prereq */))
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

  // Sort the available package fragments in the package version descending
  // order and suppress duplicate packages and, optionally, older package
  // revisions.
  //
  static void
  sort_dedup (available_packages& pfs, bool suppress_older_revisions = false)
  {
    sort (pfs.begin (), pfs.end (),
          [] (const auto& x, const auto& y)
          {
            return x.first->version > y.first->version;
          });

    pfs.erase (
      unique (pfs.begin(), pfs.end(),
              [suppress_older_revisions] (const auto& x, const auto& y)
              {
                return x.first->version.compare (y.first->version,
                                                 suppress_older_revisions) == 0;
              }),
      pfs.end ());
  }

  available_packages
  find_available (const linked_databases& dbs,
                  const package_name& name,
                  const optional<version_constraint>& c)
  {
    available_packages r;

    for (database& db: dbs)
    {
      for (shared_ptr<available_package> ap:
             pointer_result (query_available (db, name, c)))
      {
        // All repository fragments the package comes from are equally good,
        // so we pick the first unmasked one.
        //
        for (const auto& pl: ap->locations)
        {
          const lazy_shared_ptr<repository_fragment>& lrf (
            pl.repository_fragment);

          if (!rep_masked_fragment (lrf))
          {
            r.emplace_back (move (ap), lrf);
            break;
          }
        }
      }
    }

    // If there are multiple databases specified, then sort the result in the
    // package version descending order and suppress duplicates.
    //
    if (dbs.size () > 1)
      sort_dedup (r);

    // Adding a stub from the imaginary system repository to the non-empty
    // results isn't necessary but may end up with a duplicate. That's why we
    // only add it if nothing else is found.
    //
    if (r.empty ())
    {
      if (shared_ptr<available_package> ap = find_imaginary_stub (name))
        r.emplace_back (move (ap), nullptr);
    }

    return r;
  }

  available_packages
  find_available (const package_name& name,
                  const optional<version_constraint>& c,
                  const config_repo_fragments& rfs,
                  bool prereq)
  {
    available_packages r;

    for (const auto& dfs: rfs)
    {
      database& db (dfs.first);
      for (auto& af: filter (dfs.second,
                             query_available (db, name, c),
                             prereq))
      {
        r.emplace_back (
          move (af.first),
          lazy_shared_ptr<repository_fragment> (db, move (af.second)));
      }
    }

    if (rfs.size () > 1)
      sort_dedup (r);

    if (r.empty ())
    {
      if (shared_ptr<available_package> ap = find_imaginary_stub (name))
        r.emplace_back (move (ap), nullptr);
    }

    return r;
  }

  vector<shared_ptr<available_package>>
  find_available (const package_name& name,
                  const optional<version_constraint>& c,
                  const lazy_shared_ptr<repository_fragment>& rf,
                  bool prereq)
  {
    assert (!rep_masked_fragment (rf));

    vector<shared_ptr<available_package>> r;

    database& db (rf.database ());
    for (auto& ap: filter (rf.load (), query_available (db, name, c), prereq))
      r.emplace_back (move (ap));

    if (r.empty ())
    {
      if (shared_ptr<available_package> ap = find_imaginary_stub (name))
        r.emplace_back (move (ap));
    }

    return r;
  }

  pair<shared_ptr<available_package>,
       lazy_shared_ptr<repository_fragment>>
  find_available_one (const package_name& name,
                      const optional<version_constraint>& c,
                      const lazy_shared_ptr<repository_fragment>& rf,
                      bool prereq,
                      bool revision)
  {
    assert (!rep_masked_fragment (rf));

    // Filter the result based on the repository fragment to which each
    // version belongs.
    //
    database& db (rf.database ());
    auto r (
      filter_one (rf.load (),
                  query_available (db, name, c, true /* order */, revision),
                  prereq));

    if (r.first == nullptr)
      r.first = find_imaginary_stub (name);

    return make_pair (r.first,
                      (r.second != nullptr
                       ? lazy_shared_ptr<repository_fragment> (db,
                                                               move (r.second))
                       : nullptr));
  }

  pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>
  find_available_one (database& db,
                      const package_name& name,
                      const optional<version_constraint>& c,
                      const vector<shared_ptr<repository_fragment>>& rfs,
                      bool prereq,
                      bool revision)
  {
    // Filter the result based on the repository fragments to which each
    // version belongs.
    //
    auto r (
      filter_one (rfs,
                  query_available (db, name, c, true /* order */, revision),
                  prereq));

    if (r.first == nullptr)
      r.first = find_imaginary_stub (name);

    return r;
  }

  pair<shared_ptr<available_package>,
       lazy_shared_ptr<repository_fragment>>
  find_available_one (const linked_databases& dbs,
                      const package_name& name,
                      const optional<version_constraint>& c,
                      bool prereq,
                      bool revision)
  {
    for (database& db: dbs)
    {
      auto r (
        filter_one (db.load<repository_fragment> (""),
                    query_available (db, name, c, true /* order */, revision),
                    prereq));

      if (r.first != nullptr)
        return make_pair (
          move (r.first),
          lazy_shared_ptr<repository_fragment> (db, move (r.second)));
    }

    return make_pair (find_imaginary_stub (name), nullptr);
  }

  shared_ptr<available_package>
  find_available (const common_options& options,
                  database& db,
                  const shared_ptr<selected_package>& sp)
  {
    available_package_id pid (sp->name, sp->version);
    for (database& ddb: dependent_repo_configs (db))
    {
      shared_ptr<available_package> ap (ddb.find<available_package> (pid));

      if (ap != nullptr && !ap->stub ())
        return ap;
    }

    return make_available (options, db, sp);
  }

  pair<shared_ptr<available_package>,
       lazy_shared_ptr<repository_fragment>>
  find_available_fragment (const common_options& options,
                           database& db,
                           const shared_ptr<selected_package>& sp)
  {
    available_package_id pid (sp->name, sp->version);
    const string& cn (sp->repository_fragment.canonical_name ());

    for (database& ddb: dependent_repo_configs (db))
    {
      shared_ptr<available_package> ap (ddb.find<available_package> (pid));

      if (ap != nullptr && !ap->stub ())
      {
        if (shared_ptr<repository_fragment> f =
            ddb.find<repository_fragment> (cn))
        {
          if (!rep_masked_fragment (ddb, f))
            return make_pair (ap,
                              lazy_shared_ptr<repository_fragment> (ddb,
                                                                    move (f)));
        }
      }
    }

    return make_pair (find_available (options, db, sp), nullptr);
  }

  available_packages
  find_available_all (const linked_databases& dbs,
                      const package_name& name,
                      bool suppress_older_revisions)
  {
    // Collect all the databases linked explicitly and implicitly to the
    // specified databases, recursively.
    //
    // Note that this is a superset of the database cluster, since we descend
    // into the database links regardless of their types (see
    // cluster_configs() for details).
    //
    linked_databases all_dbs;
    all_dbs.reserve (dbs.size ());

    auto add = [&all_dbs] (database& db, const auto& add)
    {
      if (find (all_dbs.begin (), all_dbs.end (), db) != all_dbs.end ())
        return;

      all_dbs.push_back (db);

      {
        const linked_configs& cs (db.explicit_links ());
        for (auto i (cs.begin_linked ()); i != cs.end (); ++i)
          add (i->db, add);
      }

      {
        const linked_databases& cs (db.implicit_links ());
        for (auto i (cs.begin_linked ()); i != cs.end (); ++i)
          add (*i, add);
      }
    };

    for (database& db: dbs)
      add (db, add);

    // Collect all the available packages from all the collected databases.
    //
    available_packages r;

    for (database& db: all_dbs)
    {
      for (shared_ptr<available_package> ap:
             pointer_result (
               query_available (db, name, nullopt /* version_constraint */)))
      {
        // All repository fragments the package comes from are equally good,
        // so we pick the first unmasked one.
        //
        for (const auto& pl: ap->locations)
        {
          const lazy_shared_ptr<repository_fragment>& lrf (
            pl.repository_fragment);

          if (!rep_masked_fragment (lrf))
          {
            r.emplace_back (move (ap), lrf);
            break;
          }
        }
      }
    }

    // Sort the result in the package version descending order and suppress
    // duplicates and, if requested, older package revisions.
    //
    sort_dedup (r, suppress_older_revisions);

    return r;
  }

  pair<shared_ptr<available_package>,
       lazy_shared_ptr<repository_fragment>>
  make_available_fragment (const common_options& options,
                           database& db,
                           const shared_ptr<selected_package>& sp)
  {
    shared_ptr<available_package> ap (make_available (options, db, sp));

    if (sp->system ())
      return make_pair (move (ap), nullptr);

    // First see if we can find its repository fragment.
    //
    // Note that this is package's "old" repository fragment and there is no
    // guarantee that its dependencies are still resolvable from it. But this
    // is our best chance (we could go nuclear and point all orphans to the
    // root repository fragment but that feels a bit too drastic at the
    // moment).
    //
    // Also note that the repository information for this selected package can
    // potentially be in one of the ultimate dependent configurations as
    // determined at the time of the run when the package was configured. This
    // configurations set may differ from the current one, but let's try
    // anyway.
    //
    lazy_shared_ptr<repository_fragment> rf;
    const string& cn (sp->repository_fragment.canonical_name ());

    for (database& ddb: dependent_repo_configs (db))
    {
      if (shared_ptr<repository_fragment> f =
          ddb.find<repository_fragment> (cn))
      {
        if (!rep_masked_fragment (ddb, f))
        {
          rf = lazy_shared_ptr<repository_fragment> (ddb, move (f));
          break;
        }
      }
    }

    return make_pair (move (ap), move (rf));
  }
}
