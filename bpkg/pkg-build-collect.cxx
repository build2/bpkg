// file      : bpkg/pkg-build-collect.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-build-collect.hxx>

#include <map>
#include <set>
#include <limits>       // numeric_limits
#include <iostream>     // cout
#include <functional>   // ref()
#include <forward_list>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/satisfaction.hxx>

#include <bpkg/common-options.hxx>

#include <bpkg/cfg-link.hxx>
#include <bpkg/cfg-create.hxx>
#include <bpkg/package-query.hxx>
#include <bpkg/package-configuration.hxx>

using namespace std;

namespace bpkg
{
  // build_package
  //
  bool build_package::
  user_selection () const
  {
    return required_by.find (package_key {db.get ().main_database (), ""}) !=
           required_by.end ();
  }

  bool build_package::
  user_selection (const vector<build_package>& hold_pkgs) const
  {
    return find_if (hold_pkgs.begin (), hold_pkgs.end (),
                    [this] (const build_package& p)
                    {
                      return p.db == db && p.name () == name ();
                    }) != hold_pkgs.end ();
  }

  string build_package::
  available_name_version_db () const
  {
    const string& s (db.get ().string);
    return !s.empty ()
      ? available_name_version () + ' ' + s
      : available_name_version ();
  }

  bool build_package::
  recollect_recursively (const repointed_dependents& rpt_depts) const
  {
    assert (action                                       &&
            *action == build_package::build              &&
            available != nullptr                         &&
            selected != nullptr                          &&
            selected->state == package_state::configured &&
            selected->substate != package_substate::system);

    // Note that if the skeleton is present then the package is either being
    // already collected or its configuration has been negotiated between the
    // dependents.
    //
    return !system &&
           (dependencies                                     ||
            selected->version != available_version ()        ||
            ((!config_vars.empty () || skeleton) &&
             has_buildfile_clause (available->dependencies)) ||
            rpt_depts.find (package_key (db, name ())) != rpt_depts.end ());
  }

  bool build_package::
  reconfigure () const
  {
    assert (action && *action != drop);

    return selected != nullptr                          &&
           selected->state == package_state::configured &&
           ((flags & adjust_reconfigure) != 0 ||
            (*action == build &&
             (selected->system () != system             ||
              selected->version != available_version () ||
              (!system && (!config_vars.empty () || disfigure)))));
  }

  bool build_package::
  configure_only () const
  {
    assert (action);

    return configure_only_ ||
      (*action == build && (flags & (build_repoint | build_reevaluate)) != 0);
  }

  const version& build_package::
  available_version () const
  {
    // This should have been diagnosed before creating build_package object.
    //
    assert (available != nullptr &&
            (system
             ? available->system_version (db) != nullptr
             : !available->stub ()));

    return system ? *available->system_version (db) : available->version;
  }

  bool build_package::
  external (dir_path* d) const
  {
    assert (action);

    if (*action == build_package::drop)
      return false;

    // If adjustment or orphan, then new and old are the same.
    //
    if (available == nullptr || available->locations.empty ())
    {
      assert (selected != nullptr);

      if (selected->external ())
      {
        assert (selected->src_root);

        if (d != nullptr)
          *d = *selected->src_root;

        return true;
      }
    }
    else
    {
      const package_location& pl (available->locations[0]);

      if (pl.repository_fragment.object_id () == "") // Special root?
      {
        if (!exists (pl.location))                   // Directory case?
        {
          if (d != nullptr)
            *d = normalize (path_cast<dir_path> (pl.location), "package");

          return true;
        }
      }
      else
      {
        // See if the package comes from the directory-based repository, and
        // so is external.
        //
        // Note that such repository fragments are always preferred over
        // others (see below).
        //
        for (const package_location& pl: available->locations)
        {
          const repository_location& rl (
            pl.repository_fragment.load ()->location);

          if (rl.directory_based ())
          {
            // Note that the repository location path is always absolute for
            // the directory-based repositories but the package location may
            // potentially not be normalized. Thus, we normalize the resulting
            // path, if requested.
            //
            if (d != nullptr)
              *d = normalize (path_cast<dir_path> (rl.path () / pl.location),
                              "package");

            return true;
          }
        }
      }
    }

    return false;
  }

  void build_package::
  merge (build_package&& p)
  {
    // We don't merge objects from different configurations.
    //
    assert (db == p.db);

    // We don't merge into pre-entered objects, and from/into drops.
    //
    assert (action && *action != drop && (!p.action || *p.action != drop));

    // We never merge two repointed dependent reconfigurations.
    //
    assert ((flags & build_repoint) == 0 || (p.flags & build_repoint) == 0);

    // We never merge two existing dependent re-evaluations.
    //
    assert ((flags & build_reevaluate) == 0 ||
            (p.flags & build_reevaluate) == 0);

    // Copy the user-specified options/variables.
    //
    if (p.user_selection ())
    {
      // We don't allow a package specified on the command line multiple times
      // to have different sets of options/variables. Given that, it's
      // tempting to assert that the options/variables don't change if we
      // merge into a user selection. That's, however, not the case due to the
      // iterative plan refinement implementation details (--checkout-*
      // options and variables are only saved into the pre-entered
      // dependencies, etc.).
      //
      // Note that configuration can only be specified for packages on the
      // command line and such packages get collected/pre-entered early,
      // before any prerequisites get collected. Thus, it doesn't seem
      // possible that a package configuration/options may change after we
      // have created the package skeleton.
      //
      // Also note that if it wouldn't be true, we would potentially need to
      // re-collect the package prerequisites, since configuration change
      // could affect the enable condition evaluation and, as a result, the
      // dependency alternative choice.
      //
      assert (!skeleton ||
              ((p.config_vars.empty () || p.config_vars == config_vars) &&
               p.disfigure == disfigure));

      if (p.keep_out)
        keep_out = p.keep_out;

      if (p.disfigure)
        disfigure = p.disfigure;

      if (p.configure_only_)
        configure_only_ = p.configure_only_;

      if (p.checkout_root)
        checkout_root = move (p.checkout_root);

      if (p.checkout_purge)
        checkout_purge = p.checkout_purge;

      if (!p.config_vars.empty ())
        config_vars = move (p.config_vars);

      // Propagate the user-selection tag.
      //
      required_by.emplace (db.get ().main_database (), "");
    }

    // Copy the required-by package names only if semantics matches.
    //
    if (p.required_by_dependents == required_by_dependents)
      required_by.insert (p.required_by.begin (), p.required_by.end ());

    // Copy constraints.
    //
    // Note that we may duplicate them, but this is harmless.
    //
    constraints.insert (constraints.end (),
                        make_move_iterator (p.constraints.begin ()),
                        make_move_iterator (p.constraints.end ()));

    // Copy hold_* flags if they are "stronger".
    //
    if (!hold_package || (p.hold_package && *p.hold_package > *hold_package))
      hold_package = p.hold_package;

    if (!hold_version || (p.hold_version && *p.hold_version > *hold_version))
      hold_version = p.hold_version;

    // Copy state flags.
    //
    flags |= p.flags;

    // Upgrade dependent repointments and re-evaluations to the full builds.
    //
    if (*action == build)
      flags &= ~(build_repoint | build_reevaluate);

    // Note that we don't copy the build_package::system flag. If it was
    // set from the command line ("strong system") then we will also have
    // the '==' constraint which means that this build_package object will
    // never be replaced.
    //
    // For other cases ("weak system") we don't want to copy system over in
    // order not prevent, for example, system to non-system upgrade.
  }

  package_skeleton& build_package::
  init_skeleton (const common_options& options,
                 const shared_ptr<available_package>& override)
  {
    shared_ptr<available_package> ap (override != nullptr
                                      ? override
                                      : available);

    assert (!skeleton && ap != nullptr);

    package_key pk (db, ap->id.name);

    if (system)
    {
      // Keep the available package if its version is "close enough" to the
      // system package version. For now we will require the exact match
      // but in the future we could relax this (e.g., allow the user to
      // specify something like libfoo/^1.2.0 or some such).
      //
      const version* v (!ap->stub () ? ap->system_version (db) : nullptr);

      if (v == nullptr || *v != ap->version)
        ap = nullptr;
    }

    optional<dir_path> src_root, out_root;

    if (ap != nullptr)
    {
      src_root = external_dir ();
      out_root = (src_root && !disfigure
                  ? dir_path (db.get ().config) /= name ().string ()
                  : optional<dir_path> ());
    }

    skeleton = package_skeleton (
      options,
      move (pk),
      system,
      move (ap),
      config_vars, // @@ Maybe make optional<strings> and move?
      disfigure,
      (selected != nullptr ? &selected->config_variables : nullptr),
      move (src_root),
      move (out_root));

    return *skeleton;
  }

  // replaced_versions
  //
  void replaced_versions::
  cancel_bogus (tracer& trace, bool scratch)
  {
    bool bogus (false);
    for (auto i (begin ()); i != end (); )
    {
      const replaced_version& v (i->second);

      if (!v.replaced)
      {
        bogus = true;

        l5 ([&]{trace << "erase bogus version replacement " << i->first;});

        i = erase (i);
      }
      else
        ++i;
    }

    if (bogus && scratch)
    {
      l5 ([&]{trace << "bogus version replacement erased, throwing";});
      throw cancel_replacement ();
    }
  }

  // postponed_configuration
  //
  postponed_configuration::dependency*
  postponed_configuration::dependent_info::
  find_dependency (pair<size_t, size_t> pos)
  {
    auto i (find_if (dependencies.begin (), dependencies.end (),
                     [&pos] (const dependency& d)
                     {
                       return d.position == pos;
                     }));
    return i != dependencies.end () ? &*i : nullptr;
  }

  void postponed_configuration::dependent_info::
  add (dependency&& dep)
  {
    if (dependency* d = find_dependency (dep.position))
    {
      // Feels like we can accumulate dependencies into an existing
      // position only for an existing dependent.
      //
      assert (existing);

      for (package_key& p: dep)
      {
        // Add the dependency unless it's already there.
        //
        if (find (d->begin (), d->end (), p) == d->end ())
          d->push_back (move (p));
      }

      // Set the has_alternative flag for an existing dependent. Note that
      // it shouldn't change if already set.
      //
      if (dep.has_alternative)
      {
        if (!d->has_alternative)
          d->has_alternative = *dep.has_alternative;
        else
          assert (*d->has_alternative == *dep.has_alternative);
      }
    }
    else
      dependencies.push_back (move (dep));
  }

  void postponed_configuration::
  add (package_key&& dependent,
       bool existing,
       pair<size_t, size_t> position,
       packages&& deps,
       optional<bool> has_alternative)
  {
    assert (position.first != 0 && position.second != 0);

    add_dependencies (deps); // Don't move from since will be used later.

    auto i (dependents.find (dependent));

    if (i != dependents.end ())
    {
      dependent_info& ddi (i->second);

      ddi.add (dependency (position, move (deps), has_alternative));

      // Conceptually, on the first glance, we can only move from existing to
      // non-existing (e.g., due to a upgrade/downgrade later) and that case
      // is handled via the version replacement rollback. However, after
      // re-evaluation the existing dependent is handled similar to the new
      // dependent and we can potentially up-negotiate the dependency
      // configuration for it.
      //
      assert (ddi.existing || !existing);
    }
    else
    {
      small_vector<dependency, 1> ds ({
          dependency (position, move (deps), has_alternative)});

      dependents.emplace (move (dependent),
                          dependent_info {existing, move (ds)});
    }
  }

  bool postponed_configuration::
  contains_dependency (const packages& ds) const
  {
    for (const package_key& d: ds)
    {
      if (contains_dependency (d))
        return true;
    }

    return false;
  }

  bool postponed_configuration::
  contains_dependency (const postponed_configuration& c) const
  {
    for (const auto& d: c.dependencies)
    {
      if (contains_dependency (d))
        return true;
    }

    return false;
  }

  const pair<size_t, size_t>* postponed_configuration::
  existing_dependent_position (const package_key& p) const
  {
    const pair<size_t, size_t>* r (nullptr);

    auto i (dependents.find (p));
    if (i != dependents.end () && i->second.existing)
    {
      for (const dependency& d: i->second.dependencies)
      {
        if (r == nullptr || d.position < *r)
          r = &d.position;
      }

      assert (r != nullptr);
    }

    return r;
  }

  void postponed_configuration::
  merge (postponed_configuration&& c)
  {
    assert (c.id != id); // Can't merge to itself.

    merged_ids.push_back (c.id);

    // Merge dependents.
    //
    for (auto& d: c.dependents)
    {
      auto i (dependents.find (d.first));

      if (i != dependents.end ())
      {
        dependent_info& ddi (i->second); // Destination dependent info.
        dependent_info& sdi (d.second);  // Source dependent info.

        for (dependency& sd: sdi.dependencies)
          ddi.add (move (sd));

        // As in add() above.
        //
        assert (ddi.existing || !sdi.existing);
      }
      else
        dependents.emplace (d.first, move (d.second));
    }

    // Merge dependencies.
    //
    add_dependencies (move (c.dependencies));

    // Pick the depth of the outermost negotiated configuration (minimum
    // non-zero depth) between the two.
    //
    if (depth != 0)
    {
      if (c.depth != 0 && depth > c.depth)
        depth = c.depth;
    }
    else
      depth = c.depth;
  }

  void postponed_configuration::
  set_shadow_cluster (postponed_configuration&& c)
  {
    shadow_cluster.clear ();

    for (auto& dt: c.dependents)
    {
      positions ps;
      for (auto& d: dt.second.dependencies)
        ps.push_back (d.position);

      shadow_cluster.emplace (dt.first, move (ps));
    }
  }

  bool postponed_configuration::
  contains_in_shadow_cluster (package_key dependent,
                              pair<size_t, size_t> pos) const
  {
    auto i (shadow_cluster.find (dependent));

    if (i != shadow_cluster.end ())
    {
      const positions& ps (i->second);
      return find (ps.begin (), ps.end (), pos) != ps.end ();
    }
    else
      return false;
  }

  std::string postponed_configuration::
  string () const
  {
    std::string r;

    for (const auto& d: dependents)
    {
      r += r.empty () ? '{' : ' ';
      r += d.first.string ();

      if (d.second.existing)
        r += '^';
    }

    if (r.empty ())
      r += '{';

    r += " |";

    for (const package_key& d: dependencies)
    {
      r += ' ';
      r += d.string ();
      r += "->{";

      bool first (true);
      for (const auto& dt: dependents)
      {
        for (const dependency& dp: dt.second.dependencies)
        {
          if (find (dp.begin (), dp.end (), d) != dp.end ())
          {
            if (!first)
              r += ' ';
            else
              first = false;

            r += dt.first.string ();
            r += '/';
            r += to_string (dp.position.first);
            r += ',';
            r += to_string (dp.position.second);
          }
        }
      }

      r += '}';
    }

    r += '}';

    if (negotiated)
      r += *negotiated ? '!' : '?';

    return r;
  }

  void postponed_configuration::
  add_dependencies (packages&& deps)
  {
    for (auto& d: deps)
    {
      if (find (dependencies.begin (), dependencies.end (), d) ==
          dependencies.end ())
        dependencies.push_back (move (d));
    }
  }

  void postponed_configuration::
  add_dependencies (const packages& deps)
  {
    for (const auto& d: deps)
    {
      if (find (dependencies.begin (), dependencies.end (), d) ==
          dependencies.end ())
        dependencies.push_back (d);
    }
  }

  pair<postponed_configuration&, optional<bool>> postponed_configurations::
  add (package_key dependent,
       bool existing,
       pair<size_t, size_t> position,
       postponed_configuration::packages dependencies,
       optional<bool> has_alternative)
  {
    tracer trace ("postponed_configurations::add");

    assert (!dependencies.empty ());

    // The plan is to first go through the existing clusters and check if any
    // of them contain this dependent/dependencies in their shadow
    // clusters. If such a cluster is found, then force-add them to
    // it. Otherwise, if any dependency-intersecting clusters are present,
    // then add the specified dependent/dependencies to the one with the
    // minimum non-zero depth, if any, and to the first one otherwise.
    // Otherwise, add the new cluster. Afterwards, merge into the resulting
    // cluster other dependency-intersecting clusters. Note that in case of
    // shadow, this should normally not happen because such a cluster should
    // have been either pre-merged or its dependents should be in the
    // cluster. But it feels like it may still happen if things change, in
    // which case we will throw again (admittedly a bit fuzzy).
    //
    iterator ri;
    bool rb (true);

    // Note that if a single dependency is added, then it can only belong to a
    // single existing cluster and so no clusters merge can happen, unless we
    // are force-adding. In the later case we can only merge once for a single
    // dependency.
    //
    // Let's optimize for the common case based on these facts.
    //
    bool single (dependencies.size () == 1);

    // Merge dependency-intersecting clusters in the specified range into the
    // resulting cluster and reset change rb to false if any of the merged in
    // clusters is non-negotiated or is being negotiated.
    //
    // The iterator arguments refer to entries before and after the range
    // endpoints, respectively.
    //
    auto merge = [&trace, &ri, &rb, single, this] (iterator i,
                                                   iterator e,
                                                   bool shadow_based)
    {
      postponed_configuration& rc (*ri);

      iterator j (i);

      // Merge the intersecting configurations.
      //
      bool merged (false);
      for (++i; i != e; ++i)
      {
        postponed_configuration& c (*i);

        if (c.contains_dependency (rc))
        {
          if (!c.negotiated || !*c.negotiated)
            rb = false;

          l5 ([&]{trace << "merge " << c << " into " << rc;});

          assert (!shadow_based || (c.negotiated && *c.negotiated));

          rc.merge (move (c));
          c.dependencies.clear (); // Mark as merged from (see above).

          merged = true;

          if (single)
            break;
        }
      }

      // Erase configurations which we have merged from.
      //
      if (merged)
      {
        i = j;

        for (++i; i != e; )
        {
          if (!i->dependencies.empty ())
          {
            ++i;
            ++j;
          }
          else
            i = erase_after (j);
        }
      }
    };

    auto trace_add = [&trace, &dependent, existing, position, &dependencies]
                     (const postponed_configuration& c, bool shadow)
    {
      if (verb >= 5)
      {
        diag_record dr (trace);
        dr << "add {" << dependent;

        if (existing)
          dr << '^';

        dr << ' ' << position.first << ',' << position.second << ':';

        for (const auto& d: dependencies)
          dr << ' ' << d;

        dr << "} to " << c;

        if (shadow)
          dr << " (shadow cluster-based)";
      }
    };

    // Try to add based on the shadow cluster.
    //
    {
      auto i (begin ());
      for (; i != end (); ++i)
      {
        postponed_configuration& c (*i);

        if (c.contains_in_shadow_cluster (dependent, position))
        {
          trace_add (c, true /* shadow */);

          c.add (move (dependent),
                 existing,
                 position,
                 move (dependencies),
                 has_alternative);

          break;
        }
      }

      if (i != end ())
      {
        // Note that the cluster with a shadow cluster is by definition
        // either being negotiated or has been negotiated. Actually, there
        // is also a special case when we didn't negotiate the configuration
        // yet and are in the process of re-evaluating existing dependents.
        // Note though, that in this case we have already got the try/catch
        // frame corresponding to the cluster negotiation (see
        // collect_build_postponed() for details).
        //
        assert (i->depth != 0);

        ri = i;

        merge (before_begin (), ri, true /* shadow_based */);
        merge (ri, end (), true /* shadow_based */);

        return make_pair (ref (*ri), optional<bool> ());
      }
    }

    // Find the cluster to add the dependent/dependencies to.
    //
    optional<size_t> depth;

    auto j (before_begin ()); // Precedes iterator i.
    for (auto i (begin ()); i != end (); ++i, ++j)
    {
      postponed_configuration& c (*i);

      if (c.contains_dependency (dependencies) &&
          (!depth || (c.depth != 0 && (*depth == 0 || *depth > c.depth))))
      {
        ri = i;
        depth = c.depth;
      }
    }

    if (!depth) // No intersecting cluster?
    {
      // New cluster. Insert after the last element.
      //
      ri = insert_after (j,
                         postponed_configuration (
                           next_id_++,
                           move (dependent),
                           existing,
                           position,
                           move (dependencies),
                           has_alternative));

      l5 ([&]{trace << "create " << *ri;});
    }
    else
    {
      // Add the dependent/dependencies into an existing cluster.
      //
      postponed_configuration& c (*ri);

      trace_add (c, false /* shadow */);

      c.add (move (dependent),
             existing,
             position,
             move (dependencies),
             has_alternative);

      // Try to merge other clusters into this cluster.
      //
      merge (before_begin (), ri, false /* shadow_based */);
      merge (ri, end (), false /* shadow_based */);
    }

    return make_pair (ref (*ri), optional<bool> (rb));
  }

  void postponed_configurations::
  add (package_key dependent,
       pair<size_t, size_t> position,
       package_key dependency)
  {
    tracer trace ("postponed_configurations::add");

    // Add the new cluster to the end of the list which we can only find by
    // traversing the list. While at it, make sure that the dependency doesn't
    // belong to any existing cluster.
    //
    auto i (before_begin ()); // Insert after this element.

    for (auto j (begin ()); j != end (); ++i, ++j)
      assert (!j->contains_dependency (dependency));

    i = insert_after (i,
                      postponed_configuration (next_id_++,
                                               move (dependent),
                                               position,
                                               move (dependency)));

    l5 ([&]{trace << "create " << *i;});
  }

  postponed_configuration* postponed_configurations::
  find (size_t id)
  {
    for (postponed_configuration& cfg: *this)
    {
      if (cfg.id == id)
        return &cfg;
    }

    return nullptr;
  }

  const postponed_configuration* postponed_configurations::
  find_dependency (const package_key& d) const
  {
    for (const postponed_configuration& cfg: *this)
    {
      if (cfg.contains_dependency (d))
        return &cfg;
    }

    return nullptr;
  }

  bool postponed_configurations::
  negotiated () const
  {
    for (const postponed_configuration& cfg: *this)
    {
      if (!cfg.negotiated || !*cfg.negotiated)
        return false;
    }

    return true;
  }

  postponed_configuration& postponed_configurations::
  operator[] (size_t index)
  {
    auto i (begin ());
    for (size_t j (0); j != index; ++j, ++i) assert (i != end ());

    assert (i != end ());
    return *i;
  }

  size_t postponed_configurations::
  size () const
  {
    size_t r (0);
    for (auto i (begin ()); i != end (); ++i, ++r) ;
    return r;
  }

  // build_packages
  //
  bool build_packages::package_ref::
  operator== (const package_ref& v)
  {
    return name == v.name && db == v.db;
  }

  build_packages::
  build_packages (const build_packages& v)
      : build_package_list ()
  {
    // Copy the map.
    //
    for (const auto& p: v.map_)
      map_.emplace (p.first, data_type {end (), p.second.package});

    // Copy the list.
    //
    for (const auto& p: v)
    {
      auto i (map_.find (p.get ().db, p.get ().name ()));
      assert (i != map_.end ());
      i->second.position = insert (end (), i->second.package);
    }
  }

  build_packages& build_packages::
  operator= (build_packages&& v) noexcept (false)
  {
    clear ();

    // Move the map.
    //
    // Similar to what we do in the copy-constructor, but here we also need to
    // restore the database reference and the package shared pointers in the
    // source entry after the move. This way we can obtain the source packages
    // databases and names later while copying the list.
    //
    for (auto& p: v.map_)
    {
      build_package& bp (p.second.package);

      database&                     db (bp.db);
      shared_ptr<selected_package>  sp (bp.selected);
      shared_ptr<available_package> ap (bp.available);

      map_.emplace (p.first, data_type {end (), move (bp)});

      bp.db        = db;
      bp.selected  = move (sp);
      bp.available = move (ap);
    }

    // Copy the list.
    //
    for (const auto& p: v)
    {
      auto i (map_.find (p.get ().db, p.get ().name ()));
      assert (i != map_.end ());
      i->second.position = insert (end (), i->second.package);
    }

    return *this;
  }

  void build_packages::
  enter (package_name name, build_package pkg)
  {
    assert (!pkg.action);

    database& db (pkg.db); // Save before the move() call.
    auto p (map_.emplace (package_key {db, move (name)},
                          data_type {end (), move (pkg)}));

    assert (p.second);
  }

  build_package* build_packages::
  collect_build (const pkg_build_options& options,
                 build_package pkg,
                 const function<find_database_function>& fdb,
                 const repointed_dependents& rpt_depts,
                 const function<add_priv_cfg_function>& apc,
                 bool initial_collection,
                 replaced_versions& replaced_vers,
                 postponed_configurations& postponed_cfgs,
                 build_package_refs* dep_chain,
                 postponed_packages* postponed_repo,
                 postponed_packages* postponed_alts,
                 postponed_dependencies* postponed_deps,
                 postponed_positions* postponed_poss,
                 unacceptable_alternatives* unacceptable_alts,
                 const function<verify_package_build_function>& vpb)
  {
    using std::swap; // ...and not list::swap().

    tracer trace ("collect_build");

    // See the above notes.
    //
    bool recursive (dep_chain != nullptr);
    assert ((postponed_repo    != nullptr) == recursive &&
            (postponed_alts    != nullptr) == recursive &&
            (postponed_deps    != nullptr) == recursive &&
            (postponed_poss    != nullptr) == recursive &&
            (unacceptable_alts != nullptr) == recursive);

    // Only builds are allowed here.
    //
    assert (pkg.action && *pkg.action == build_package::build &&
            pkg.available != nullptr);

    package_key pk (pkg.db, pkg.available->id.name);

    // Apply the version replacement, if requested, and indicate that it was
    // applied.
    //
    auto vi (replaced_vers.find (pk));

    if (vi != replaced_vers.end () && !vi->second.replaced)
    {
      l5 ([&]{trace << "apply version replacement for "
                    << pkg.available_name_version_db ();});

      replaced_version& v (vi->second);

      v.replaced = true;

      if (v.available != nullptr)
      {
        pkg.available = v.available;
        pkg.repository_fragment = v.repository_fragment;
        pkg.system = v.system;

        l5 ([&]{trace << "replacement: "
                      << pkg.available_name_version_db ();});
      }
      else
      {
        l5 ([&]{trace << "replacement: drop";});

        assert (pkg.selected != nullptr);

        collect_drop (options, pkg.db, pkg.selected, replaced_vers);
        return nullptr;
      }
    }

    // Add the version replacement entry, call the verification function if
    // specified, and throw replace_version.
    //
    auto replace_ver = [&pk, &vpb, &vi, &replaced_vers]
      (const build_package& p)
      {
        replaced_version rv (p.available, p.repository_fragment, p.system);

        if (vi != replaced_vers.end ())
          vi->second = move (rv);
        else
          replaced_vers.emplace (move (pk), move (rv));

        if (vpb)
          vpb (p, true /* scratch */);

        throw replace_version ();
      };

    auto i (map_.find (pk));

    // If we already have an entry for this package name, then we have to
    // pick one over the other.
    //
    // If the existing entry is a drop, then we override it. If the existing
    // entry is a pre-entered or is non-build one, then we merge it into the
    // new build entry. Otherwise (both are builds), we pick one and merge
    // the other into it.
    //
    if (i != map_.end ())
    {
      build_package& bp (i->second.package);

      // Note that we used to think that the scenario when the build could
      // replace drop could never happen since we would start collecting
      // from scratch. This has changed when we introduced replaced_versions
      // for collecting drops.
      //
      if (bp.action && *bp.action == build_package::drop)        // Drop.
      {
        bp = move (pkg);
      }
      else if (!bp.action || *bp.action != build_package::build) // Non-build.
      {
        pkg.merge (move (bp));
        bp = move (pkg);
      }
      else                                                       // Build.
      {
        // At the end we want p1 to point to the object that we keep
        // and p2 to the object that we merge from.
        //
        build_package* p1 (&bp);
        build_package* p2 (&pkg);

        // Pick with the following preference order: user selection over
        // implicit one, source package over a system one, newer version
        // over an older one. So get the preferred into p1 and the other
        // into p2.
        //
        {
          int us (p1->user_selection () - p2->user_selection ());
          int sf (p1->system - p2->system);

          if (us < 0              ||
              (us == 0 && sf > 0) ||
              (us == 0 &&
               sf == 0 &&
               p2->available_version () > p1->available_version ()))
            swap (p1, p2);
        }

        // If the versions differ, pick the satisfactory one and if both are
        // satisfactory, then keep the preferred.
        //
        if (p1->available_version () != p2->available_version ())
        {
          using constraint_type = build_package::constraint_type;

          // See if pv's version satisfies pc's constraints. Return the
          // pointer to the unsatisfied constraint or NULL if all are
          // satisfied.
          //
          auto test = [] (build_package* pv,
                          build_package* pc) -> const constraint_type*
          {
            for (const constraint_type& c: pc->constraints)
            {
              if (!satisfies (pv->available_version (), c.value))
                return &c;
            }

            return nullptr;
          };

          // First see if p1 satisfies p2's constraints.
          //
          if (auto c2 = test (p1, p2))
          {
            // If not, try the other way around.
            //
            if (auto c1 = test (p2, p1))
            {
              const package_name& n  (i->first.name);
              const string&       d1 (c1->dependent);
              const string&       d2 (c2->dependent);

              fail << "unable to satisfy constraints on package " << n <<
                info << d1 << c1->db << " depends on (" << n << " "
                   << c1->value << ")" <<
                info << d2 << c2->db << " depends on (" << n << " "
                   << c2->value << ")" <<
                info << "available " << p1->available_name_version () <<
                info << "available " << p2->available_name_version () <<
                info << "explicitly specify " << n << " version to manually "
                   << "satisfy both constraints";
            }
            else
              swap (p1, p2);
          }

          l4 ([&]{trace << "pick " << p1->available_name_version_db ()
                        << " over " << p2->available_name_version_db ();});
        }

        // See if we are replacing the object. If not, then we don't need to
        // collect its prerequisites since that should have already been
        // done. Remember, p1 points to the object we want to keep.
        //
        bool replace (p1 != &i->second.package);

        if (replace)
        {
          swap (*p1, *p2);
          swap (p1, p2); // Setup for merge below.
        }

        p1->merge (move (*p2));

        if (replace)
        {
          if (p1->available_version () != p2->available_version () ||
              p1->system != p2->system)
          {
            // See if in-place replacement is possible (no dependencies, etc)
            // and set scratch to false if that's the case.
            //
            // Firstly, such a package should not participate in any
            // configuration negotiation.
            //
            // Other than that, it looks like the only optimization we can do
            // easily is if the package has no dependencies (and thus cannot
            // impose any constraints). Anything more advanced would require
            // analyzing our dependencies (which we currently cannot easily
            // get) and (1) either dropping the dependency build_package
            // altogether if we are the only dependent (so that it doesn't
            // influence any subsequent dependent) or (2) making sure our
            // constraint is a sub-constraint of any other constraint and
            // removing it from the dependency build_package.  Maybe/later.
            //
            // NOTE: remember to update collect_drop() if changing anything
            // here.
            //
            bool scratch (true);

            // While checking if the package has any dependencies skip the
            // toolchain build-time dependencies since they should be quite
            // common.
            //
            if (!has_dependencies (options, p2->available->dependencies))
              scratch = false;

            l5 ([&]{trace << p2->available_name_version_db ()
                          << " package version needs to be replaced "
                          << (!scratch ? "in-place " : "") << "with "
                          << p1->available_name_version_db ();});

            if (scratch)
              replace_ver (*p1);
          }
          else
          {
            // It doesn't seem possible that replacing the build object
            // without changing the package version may result in changing the
            // package configuration since the configuration always gets into
            // the initial package build entry (potentially pre-entered,
            // etc). If it wouldn't be true then we would also need to add the
            // replacement version entry and re-collect from scratch.
          }
        }
        else
          return nullptr;
      }
    }
    else
    {
      // Treat the replacement of the existing dependent that is participating
      // in the configuration negotiation also as a version replacement. This
      // way we will not be treating the dependent as an existing on the
      // re-collection (see query_existing_dependents() for details).
      //
      // Note: an existing dependent may not be configured as system.
      //
      if (pkg.selected != nullptr &&
          (pkg.selected->version != pkg.available_version () ||
           pkg.system))
      {

        for (const postponed_configuration& cfg: postponed_cfgs)
        {
          auto i (cfg.dependents.find (pk));

          if (i != cfg.dependents.end () && i->second.existing)
            replace_ver (pkg);
        }
      }

      // This is the first time we are adding this package name to the map.
      //
      l4 ([&]{trace << "add " << pkg.available_name_version_db ();});

      i = map_.emplace (move (pk), data_type {end (), move (pkg)}).first;
    }

    build_package& p (i->second.package);

    if (vpb)
      vpb (p, false /* scratch */);

    // Recursively collect build prerequisites, if requested.
    //
    // Note that detecting dependency cycles during the satisfaction phase
    // would be premature since they may not be present in the final package
    // list. Instead we check for them during the ordering phase.
    //
    // The question, of course, is whether we can still end up with an
    // infinite recursion here? Note that for an existing map entry we only
    // recurse after the entry replacement. The infinite recursion would mean
    // that we may replace a package in the map with the same version multiple
    // times:
    //
    // ... p1 -> p2 -> ... p1
    //
    // Every replacement increases the entry version and/or tightens the
    // constraints the next replacement will need to satisfy. It feels
    // impossible that a package version can "return" into the map being
    // replaced once. So let's wait until some real use case proves this
    // reasoning wrong.
    //
    if (recursive)
      collect_build_prerequisites (options,
                                   p,
                                   fdb,
                                   rpt_depts,
                                   apc,
                                   initial_collection,
                                   replaced_vers,
                                   *dep_chain,
                                   postponed_repo,
                                   postponed_alts,
                                   0 /* max_alt_index */,
                                   *postponed_deps,
                                   postponed_cfgs,
                                   *postponed_poss,
                                   *unacceptable_alts);

    return &p;
  }

  void build_packages::
  collect_build_prerequisites (const pkg_build_options& options,
                               build_package& pkg,
                               const function<find_database_function>& fdb,
                               const repointed_dependents& rpt_depts,
                               const function<add_priv_cfg_function>& apc,
                               bool initial_collection,
                               replaced_versions& replaced_vers,
                               build_package_refs& dep_chain,
                               postponed_packages* postponed_repo,
                               postponed_packages* postponed_alts,
                               size_t max_alt_index,
                               postponed_dependencies& postponed_deps,
                               postponed_configurations& postponed_cfgs,
                               postponed_positions& postponed_poss,
                               unacceptable_alternatives& unacceptable_alts,
                               pair<size_t, size_t> reeval_pos)
  {
    // NOTE: don't forget to update collect_build_postponed() if changing
    // anything in this function.
    //
    tracer trace ("collect_build_prerequisites");

    assert (pkg.action && *pkg.action == build_package::build);

    const package_name& nm (pkg.name ());
    database& pdb (pkg.db);
    package_key pk (pdb, nm);

    bool reeval (reeval_pos.first != 0);

    // The being re-evaluated dependent cannot be recursively collected yet.
    // Also, we don't expect it being configured as system.
    //
    // Note that the configured package can still be re-evaluated after
    // collect_build_prerequisites() has been called but didn't end up with
    // the recursive collection.
    //
    assert (!reeval ||
            ((!pkg.recursive_collection ||
              !pkg.recollect_recursively (rpt_depts)) &&
             !pkg.skeleton && !pkg.system));

    // If this package is not being re-evaluated, is not yet collected
    // recursively, needs to be reconfigured, and is not yet postponed, then
    // check if it is a dependency of any dependent with configuration clause
    // and postpone the collection if that's the case.
    //
    // The reason why we don't need to do this for the re-evaluated case is as
    // follows: this logic is used for an existing dependent that is not
    // otherwise built (e.g., reconfigured) which means its externally-
    // imposed configuration (user, dependents) is not being changed.
    //
    if (!reeval                   &&
        !pkg.recursive_collection &&
        pkg.reconfigure ()        &&
        postponed_cfgs.find_dependency (pk) == nullptr)
    {
      // If the dependent is being built, then check if it was re-evaluated to
      // the position greater than the dependency position. Return true if
      // that's the case, so this package is added to the resulting list and
      // we can handle this situation below.
      //
      // Note that we rely on "small function object" optimization here.
      //
      const function<verify_dependent_build_function> verify (
        [&postponed_cfgs]
        (const package_key& pk, pair<size_t, size_t> pos)
        {
          for (const postponed_configuration& cfg: postponed_cfgs)
          {
            if (cfg.negotiated)
            {
              if (const pair<size_t, size_t>* p =
                  cfg.existing_dependent_position (pk))
              {
                if (p->first > pos.first)
                  return true;
              }
            }
          }

          return false;
        });

      // Note that there can be multiple existing dependents for a dependency.
      // Strictly speaking, we only need to add the first one with the
      // assumption that the remaining dependents will also be considered
      // comes the time for the negotiation. Let's, however, process all of
      // them to detect the potential "re-evaluation on the greater dependency
      // index" situation earlier. And, generally, have as much information as
      // possible up front.
      //
      vector<existing_dependent> eds (
        query_existing_dependents (trace,
                                   pk.db,
                                   pk.name,
                                   replaced_vers,
                                   rpt_depts,
                                   verify));

      if (!eds.empty ())
      {
        for (existing_dependent& ed: eds)
        {
          package_key dpk (ed.db, ed.selected->name);
          size_t& di (ed.dependency_position.first);

          const build_package* bp (&pkg);

          // Check if this dependent needs to be re-evaluated to an earlier
          // dependency position and, if that's the case, create the
          // configuration cluster with this dependency instead.
          //
          // Note that if the replace flag is false, we proceed normally with
          // the assumption that the dependency referred by the entry will be
          // collected later and its configuration cluster will be created
          // normally and will be negotiated earlier than the cluster being
          // created for the current dependency (see collect_build_postponed()
          // for details).
          //
          {
            auto pi (postponed_poss.find (dpk));

            if (pi != postponed_poss.end () && pi->second.first < di)
            {
              // If requested, override the first encountered non-replace
              // position to replace. See collect_build_postponed () for
              // details.
              //
              if (!pi->second.replace && postponed_poss.replace)
              {
                pi->second.replace = true;
                postponed_poss.replace = false;
              }

              if (pi->second.replace)
              {
                // Overwrite the existing dependent dependency information and
                // fall through to proceed as for the normal case.
                //
                bp = replace_existing_dependent_dependency (
                  trace,
                  options,
                  ed, // Note: modified.
                  pi->second,
                  fdb,
                  rpt_depts,
                  apc,
                  initial_collection,
                  replaced_vers,
                  postponed_cfgs);

                pk = package_key (bp->db, bp->name ());

                // Note that here we side-step the bogus logic (by not setting
                // the skipped flag) because in this case (replace=true) our
                // choices are either (potentially) bogus or pathological
                // (where we have evaluated too far). In other words, the
                // postponed entry may cause the depends entry that triggered
                // it to disappear (and thus, strictly speaking, to become
                // bogus) but if we cancel it, we will be back to square one.
              }
            }
          }

          // Make sure that this existing dependent doesn't belong to any
          // (being) negotiated configuration cluster with a greater
          // dependency index. That would mean that this dependent has already
          // been re-evaluated to this index and so cannot participate in the
          // configuration negotiation of this earlier dependency.
          //
          for (const postponed_configuration& cfg: postponed_cfgs)
          {
            if (const pair<size_t, size_t>* p =
                cfg.existing_dependent_position (pk))
            {
              size_t ei (p->first);

              if (di < ei && cfg.negotiated)
              {
                // Feels like there cannot be an earlier position.
                //
                postponed_position pp (ed.dependency_position,
                                       false /* replace */);

                auto p (postponed_poss.emplace (move (pk), pp));
                if (!p.second)
                {
                  assert (p.first->second > pp);
                  p.first->second = pp;
                }

                l5 ([&]{trace << "cannot cfg-postpone dependency "
                              << bp->available_name_version_db ()
                              << " of existing dependent " << *ed.selected
                              << ed.db << " (index " << di
                              << ") due to earlier dependency index " << ei
                              << " in " << cfg << ", throwing "
                              << "postpone_position";});

                // Don't print the "while satisfying..." chain.
                //
                dep_chain.clear ();

                throw postpone_position ();
              }

              if (di == ei)
              {
                // For the negotiated cluster all the dependency packages
                // should have been added. For non-negotiated cluster we
                // cannot add the missing dependencies at the moment and will
                // do it as a part of the dependent re-evaluation.
                //
                assert (!cfg.negotiated);
              }
            }
          }

          l5 ([&]{trace << "cfg-postpone dependency "
                        << bp->available_name_version_db ()
                        << " of existing dependent " << *ed.selected
                        << ed.db;});

          postponed_cfgs.add (move (dpk), ed.dependency_position, move (pk));
        }

        return;
      }
    }

    pkg.recursive_collection = true;

    if (pkg.system)
    {
      l5 ([&]{trace << "skip system " << pkg.available_name_version_db ();});
      return;
    }

    const shared_ptr<available_package>& ap (pkg.available);
    assert (ap != nullptr);

    const shared_ptr<selected_package>& sp (pkg.selected);

    // True if this is an up/down-grade.
    //
    bool ud (sp != nullptr && sp->version != pkg.available_version ());

    // If this is a repointed dependent, then it points to its prerequisite
    // replacements flag map (see repointed_dependents for details).
    //
    const map<package_key, bool>* rpt_prereq_flags (nullptr);

    // Bail out if this is a configured non-system package and no recursive
    // collection is required.
    //
    bool src_conf (sp != nullptr &&
                   sp->state == package_state::configured &&
                   sp->substate != package_substate::system);

    // The being re-evaluated dependent must be configured as a source package
    // and should not be collected recursively (due to upgrade, etc).
    //
    assert (!reeval || (src_conf && !pkg.recollect_recursively (rpt_depts)));

    if (src_conf)
    {
      repointed_dependents::const_iterator i (rpt_depts.find (pk));

      if (i != rpt_depts.end ())
        rpt_prereq_flags = &i->second;

      if (!reeval && !pkg.recollect_recursively (rpt_depts))
      {
        l5 ([&]{trace << "skip configured "
                      << pkg.available_name_version_db ();});
        return;
      }
    }

    // Iterate over dependencies, trying to unambiguously select a
    // satisfactory dependency alternative for each of them. Fail or postpone
    // the collection if unable to do so.
    //
    const dependencies& deps (ap->dependencies);

    // The skeleton can be pre-initialized before the recursive collection
    // starts (as a part of dependency configuration negotiation, etc). The
    // dependencies and alternatives members must both be either present or
    // not.
    //
    assert ((!pkg.dependencies || pkg.skeleton) &&
            pkg.dependencies.has_value () == pkg.alternatives.has_value ());

    // Note that the selected alternatives list can be filled partially (see
    // build_package::dependencies for details). In this case we continue
    // collecting where we stopped previously.
    //
    if (!pkg.dependencies)
    {
      l5 ([&]{trace << (reeval ? "reeval " : "begin ")
                    << pkg.available_name_version_db ();});

      pkg.dependencies = dependencies ();
      pkg.alternatives = vector<size_t> ();

      if (size_t n = deps.size ())
      {
        pkg.dependencies->reserve (n);
        pkg.alternatives->reserve (n);
      }

      if (!pkg.skeleton)
        pkg.init_skeleton (options);
    }
    else
      l5 ([&]{trace << "resume " << pkg.available_name_version_db ();});

    dependencies&   sdeps (*pkg.dependencies);
    vector<size_t>& salts (*pkg.alternatives);

    assert (sdeps.size () == salts.size ()); // Must be parallel.

    // Check if there is nothing to collect anymore.
    //
    if (sdeps.size () == deps.size ())
    {
      l5 ([&]{trace << "end " << pkg.available_name_version_db ();});
      return;
    }

    // Show how we got here if things go wrong.
    //
    // To suppress printing this information clear the dependency chain before
    // throwing an exception.
    //
    auto g (
      make_exception_guard (
        [&dep_chain] ()
        {
          // Note that we also need to clear the dependency chain, to prevent
          // the caller's exception guard from printing it.
          //
          while (!dep_chain.empty ())
          {
            info << "while satisfying "
                 << dep_chain.back ().get ().available_name_version_db ();

            dep_chain.pop_back ();
          }
        }));

    dep_chain.push_back (pkg);

    assert (sdeps.size () < deps.size ());

    package_skeleton& skel (*pkg.skeleton);

    auto fail_reeval = [&pkg] ()
      {
        fail << "unable to re-create dependency information of already "
        << "configured package " << pkg.available_name_version_db () <<
        info << "likely cause is change in external environment" <<
        info << "consider resetting the build configuration";
      };

    bool postponed (false);
    bool reevaluated (false);

    for (size_t di (sdeps.size ()); di != deps.size (); ++di)
    {
      // Fail if we missed the re-evaluation target position for any reason.
      //
      if (reeval && di == reeval_pos.first) // Note: reeval_pos is 1-based.
        fail_reeval ();

      const dependency_alternatives_ex& das (deps[di]);

      // Add an empty alternatives list into the selected dependency list if
      // this is a toolchain build-time dependency.
      //
      dependency_alternatives_ex sdas (das.buildtime, das.comment);

      if (toolchain_buildtime_dependency (options, das, &nm))
      {
        sdeps.push_back (move (sdas));
        salts.push_back (0);           // Keep parallel to sdeps.
        continue;
      }

      // Evaluate alternative conditions and filter enabled alternatives. Add
      // an empty alternatives list into the selected dependency list if there
      // are none.
      //
      build_package::dependency_alternatives_refs edas;

      if (pkg.postponed_dependency_alternatives)
      {
        edas = move (*pkg.postponed_dependency_alternatives);
        pkg.postponed_dependency_alternatives = nullopt;
      }
      else
      {
        for (size_t i (0); i != das.size (); ++i)
        {
          const dependency_alternative& da (das[i]);

          if (!da.enable ||
              skel.evaluate_enable (*da.enable, make_pair (di, i)))
            edas.push_back (make_pair (ref (da), i));
        }
      }

      if (edas.empty ())
      {
        sdeps.push_back (move (sdas));
        salts.push_back (0);           // Keep parallel to sdeps.
        continue;
      }

      // Try to pre-collect build information (pre-builds) for the
      // dependencies of an alternative. Optionally, issue diagnostics into
      // the specified diag record. In the dry-run mode don't change the
      // packages collection state (postponed_repo set, etc).
      //
      // Note that rather than considering an alternative as unsatisfactory
      // (returning no pre-builds) the function can fail in some cases
      // (multiple possible configurations for a build-time dependency, orphan
      // or broken selected package, etc). The assumption here is that the
      // user would prefer to fix a dependency-related issue first instead of
      // proceeding with the build which can potentially end up with some less
      // preferable dependency alternative.
      //
      struct prebuild
      {
        bpkg::dependency                           dependency;
        reference_wrapper<database>                db;
        shared_ptr<selected_package>               selected;
        shared_ptr<available_package>              available;
        lazy_shared_ptr<bpkg::repository_fragment> repository_fragment;
        bool                                       system;
        bool                                       specified_dependency;
        bool                                       force;

        // True if the dependency package is either selected in the
        // configuration or is already being built.
        //
        bool                                       reused;
      };
      using prebuilds = small_vector<prebuild, 1>;

      class precollect_result
      {
      public:
        // Nullopt if some dependencies cannot be resolved.
        //
        optional<prebuilds> builds;

        // If some dependency of the alternative cannot be resolved because
        // there is no version available which can satisfy all the being built
        // dependents, then this member contains all the dependency builds
        // (which otherwise would be contained in the builds member).
        //
        optional<prebuilds> unsatisfactory;

        // True if dependencies can all be resolved (builds is present) and
        // are all reused (see above).
        //
        bool reused = false;

        // True if some of the dependencies cannot be resolved (builds is
        // nullopt) and the dependent package prerequisites collection needs
        // to be postponed due to inability to find a version satisfying the
        // pre-entered constraint from repositories available to the dependent
        // package.
        //
        bool repo_postpone = false;

        // Create precollect result containing dependency builds.
        //
        precollect_result (prebuilds&& bs, bool r)
            : builds (move (bs)), reused (r) {}

        // Create precollect result containing unsatisfactory dependency
        // builds.
        //
        precollect_result (bool r, prebuilds&& bs)
            : unsatisfactory (move (bs)), reused (r) {}

        // Create precollect result without builds (some dependency can't be
        // resolved, etc).
        //
        explicit
        precollect_result (bool p): repo_postpone (p) {}
      };

      auto precollect = [&options,
                         &pkg,
                         &nm,
                         &pdb,
                         ud,
                         &fdb,
                         rpt_prereq_flags,
                         &apc,
                         postponed_repo,
                         &dep_chain,
                         &trace,
                         this]
        (const dependency_alternative& da,
         bool buildtime,
         const package_prerequisites* prereqs,
         diag_record* dr = nullptr,
         bool dry_run = false) -> precollect_result
        {
          prebuilds r;
          bool reused (true);

          const lazy_shared_ptr<repository_fragment>& af (
            pkg.repository_fragment);

          for (const dependency& dp: da)
          {
            const package_name& dn (dp.name);

            if (buildtime && pdb.type == build2_config_type)
            {
              assert (dr == nullptr); // Should fail on the "silent" run.

              // Note that the dependent is not necessarily a build system
              // module.
              //
              fail << "build-time dependency " << dn << " in build system "
                   << "module configuration" <<
                info << "build system modules cannot have build-time "
                   << "dependencies";
            }

            bool system    (false);
            bool specified (false);

            // If the user specified the desired dependency version
            // constraint, then we will use it to overwrite the constraint
            // imposed by the dependent package, checking that it is still
            // satisfied.
            //
            // Note that we can't just rely on the execution plan refinement
            // that will pick up the proper dependency version at the end of
            // the day. We may just not get to the plan execution simulation,
            // failing due to inability for dependency versions collected by
            // two dependents to satisfy each other constraints (for an
            // example see the
            // pkg-build/dependency/apply-constraints/resolve-conflict/
            // tests).

            // Points to the desired dependency version constraint, if
            // specified, and is NULL otherwise. Can be used as boolean flag.
            //
            const version_constraint* dep_constr (nullptr);

            database* ddb (fdb (pdb, dn, buildtime));

            auto i (ddb != nullptr
                    ? map_.find (*ddb, dn)
                    : map_.find_dependency (pdb, dn, buildtime));

            if (i != map_.end ())
            {
              const build_package& bp (i->second.package);

              specified = !bp.action; // Is pre-entered.

              if (specified &&
                  //
                  // The version constraint is specified,
                  //
                  bp.hold_version && *bp.hold_version)
              {
                assert (bp.constraints.size () == 1);

                const build_package::constraint_type& c (bp.constraints[0]);

                dep_constr = &c.value;
                system = bp.system;

                // If the user-specified dependency constraint is the wildcard
                // version, then it satisfies any dependency constraint.
                //
                if (!wildcard (*dep_constr) &&
                    !satisfies (*dep_constr, dp.constraint))
                {
                  if (dr != nullptr)
                    *dr << error << "unable to satisfy constraints on package "
                        << dn <<
                      info << nm << pdb << " depends on (" << dn
                           << " " << *dp.constraint << ")" <<
                      info << c.dependent << c.db << " depends on (" << dn
                           << " " << c.value << ")" <<
                      info << "specify " << dn << " version to satisfy " << nm
                           << " constraint";

                  return precollect_result (false /* postpone */);
                }
              }
            }

            const dependency& d (!dep_constr
                                 ? dp
                                 : dependency {dn, *dep_constr});

            // First see if this package is already selected. If we already
            // have it in the configuration and it satisfies our dependency
            // version constraint, then we don't want to be forcing its
            // upgrade (or, worse, downgrade).
            //
            // If the prerequisite configuration is explicitly specified by
            // the user, then search for the prerequisite in this specific
            // configuration. Otherwise, search recursively in the explicitly
            // linked configurations of the dependent configuration.
            //
            // Note that for the repointed dependent we will always find the
            // prerequisite replacement rather than the prerequisite being
            // replaced.
            //
            pair<shared_ptr<selected_package>, database*> spd (
              ddb != nullptr
              ? make_pair (ddb->find<selected_package> (dn), ddb)
              : find_dependency (pdb, dn, buildtime));

            if (ddb == nullptr)
              ddb = &pdb;

            shared_ptr<selected_package>& dsp (spd.first);

            if (prereqs != nullptr &&
                (dsp == nullptr ||
                 find_if (prereqs->begin (), prereqs->end (),
                          [&dsp] (const auto& v)
                          {
                            return v.first.object_id () == dsp->name;
                          }) == prereqs->end ()))
              return precollect_result (false /* postpone */);

            pair<shared_ptr<available_package>,
                 lazy_shared_ptr<repository_fragment>> rp;

            shared_ptr<available_package>& dap (rp.first);

            bool force (false);

            if (dsp != nullptr)
            {
              // Switch to the selected package configuration.
              //
              ddb = spd.second;

              // If we are collecting prerequisites of the repointed
              // dependent, then only proceed further if this is either a
              // replacement or unamended prerequisite and we are
              // up/down-grading (only for the latter).
              //
              if (rpt_prereq_flags != nullptr)
              {
                auto i (rpt_prereq_flags->find (package_key {*ddb, dn}));

                bool unamended   (i == rpt_prereq_flags->end ());
                bool replacement (!unamended && i->second);

                // We can never end up with the prerequisite being replaced,
                // since the fdb() function should always return the
                // replacement instead (see above).
                //
                assert (unamended || replacement);

                if (!(replacement || (unamended && ud)))
                  continue;
              }

              if (dsp->state == package_state::broken)
              {
                assert (dr == nullptr); // Should fail on the "silent" run.

                fail << "unable to build broken package " << dn << *ddb <<
                  info << "use 'pkg-purge --force' to remove";
              }

              // If the constraint is imposed by the user we also need to make
              // sure that the system flags are the same.
              //
              if (satisfies (dsp->version, d.constraint) &&
                  (!dep_constr || dsp->system () == system))
              {
                system = dsp->system ();

                version_constraint vc (dsp->version);

                // First try to find an available package for this exact
                // version, falling back to ignoring version revision and
                // iteration. In particular, this handles the case where a
                // package moves from one repository to another (e.g., from
                // testing to stable). For a system package we will try to
                // find the available package that matches the selected
                // package version (preferable for the configuration
                // negotiation machinery) and, if fail, fallback to picking
                // the latest one (its exact version doesn't really matter in
                // this case).
                //
                // It seems reasonable to search for the package in the
                // repositories explicitly added by the user if the selected
                // package was explicitly specified on command line, and in
                // the repository (and its complements/prerequisites) of the
                // dependent being currently built otherwise.
                //
                if (dsp->hold_package)
                {
                  linked_databases dbs (dependent_repo_configs (*ddb));

                  rp = find_available_one (dbs,
                                           dn,
                                           vc,
                                           true /* prereq */,
                                           true /* revision */);

                  if (dap == nullptr)
                    rp = find_available_one (dbs, dn, vc);

                  if (dap == nullptr && system)
                    rp = find_available_one (dbs, dn, nullopt);
                }
                else if (af != nullptr)
                {
                  rp = find_available_one (dn,
                                           vc,
                                           af,
                                           true /* prereq */,
                                           true /* revision */);

                  if (dap == nullptr)
                    rp = find_available_one (dn, vc, af);

                  if (dap == nullptr && system)
                    rp = find_available_one (dn, nullopt, af);
                }

                // A stub satisfies any version constraint so we weed them out
                // (returning stub as an available package feels wrong).
                //
                if (dap == nullptr || dap->stub ())
                  rp = make_available_fragment (options, *ddb, dsp);
              }
              else
                // Remember that we may be forcing up/downgrade; we will deal
                // with it below.
                //
                force = true;
            }

            // If this is a build-time dependency and we build it for the
            // first time, then we need to find a suitable configuration (of
            // the host or build2 type) to build it in.
            //
            // If the current configuration (ddb) is of the suitable type,
            // then we use that. Otherwise, we go through its immediate
            // explicit links. If only one of them has the suitable type, then
            // we use that. If there are multiple of them, then we fail
            // advising the user to pick one explicitly. If there are none,
            // then we create the private configuration and use that. If the
            // current configuration is private, then search/create in the
            // parent configuration instead.
            //
            // Note that if the user has explicitly specified the
            // configuration for this dependency on the command line (using
            // --config-*), then this configuration is used as the starting
            // point for this search.
            //
            if (buildtime      &&
                dsp == nullptr &&
                ddb->type != buildtime_dependency_type (dn))
            {
              database*  db (nullptr);
              database& sdb (ddb->private_ () ? ddb->parent_config () : *ddb);

              const string& type (buildtime_dependency_type (dn));

              // Skip the self-link.
              //
              const linked_configs& lcs (sdb.explicit_links ());
              for (auto i (lcs.begin_linked ()); i != lcs.end (); ++i)
              {
                database& ldb (i->db);

                if (ldb.type == type)
                {
                  if (db == nullptr)
                    db = &ldb;
                  else
                  {
                    assert (dr == nullptr); // Should fail on the "silent" run.

                    fail << "multiple possible " << type << " configurations "
                         << "for build-time dependency (" << dp << ")" <<
                      info << db->config_orig <<
                      info << ldb.config_orig <<
                      info << "use --config-* to select the configuration";
                  }
                }
              }

              // If no suitable configuration is found, then create and link
              // it, unless the --no-private-config options is specified. In
              // the latter case, print the dependency chain to stdout and
              // exit with the specified code.
              //
              if (db == nullptr)
              {
                // The private config should be created on the "silent" run
                // and so there always should be a suitable configuration on
                // the diagnostics run.
                //
                assert (dr == nullptr);

                if (options.no_private_config_specified ())
                try
                {
                  // Note that we don't have the dependency package version
                  // yet. We could probably rearrange the code and obtain the
                  // available dependency package by now, given that it comes
                  // from the main database and may not be specified as system
                  // (we would have the configuration otherwise). However,
                  // let's not complicate the code further and instead print
                  // the package name and the constraint, if present.
                  //
                  // Also, in the future, we may still need the configuration
                  // to obtain the available dependency package for some
                  // reason (may want to fetch repositories locally, etc).
                  //
                  cout << d << '\n';

                  // Note that we also need to clean the dependency chain, to
                  // prevent the exception guard from printing it to stderr.
                  //
                  for (build_package_refs dc (move (dep_chain));
                       !dc.empty (); )
                  {
                    const build_package& p (dc.back ());

                    cout << p.available_name_version () << ' '
                         << p.db.get ().config << '\n';

                    dc.pop_back ();
                  }

                  throw failed (options.no_private_config ());
                }
                catch (const io_error&)
                {
                  fail << "unable to write to stdout";
                }

                const strings mods {"cc"};

                const strings vars {
                  "config.config.load=~" + type,
                    "config.config.persist+='config.*'@unused=drop"};

                dir_path cd (bpkg_dir / dir_path (type));

                // Wipe a potentially existing un-linked private configuration
                // left from a previous faulty run. Note that trying to reuse
                // it would be a bad idea since it can be half-prepared, with
                // an outdated database schema version, etc.
                //
                cfg_create (options,
                            sdb.config_orig / cd,
                            optional<string> (type) /* name */,
                            type                    /* type */,
                            mods,
                            vars,
                            false                   /* existing */,
                            true                    /* wipe */);

                // Note that we will copy the name from the configuration
                // unless it clashes with one of the existing links.
                //
                shared_ptr<configuration> lc (
                  cfg_link (sdb,
                            sdb.config / cd,
                            true    /* relative */,
                            nullopt /* name */,
                            true    /* sys_rep */));

                // Save the newly-created private configuration, together with
                // the containing configuration database, for their subsequent
                // re-link.
                //
                apc (sdb, move (cd));

                db = &sdb.find_attached (*lc->id);
              }

              ddb = db; // Switch to the dependency configuration.
            }

            // Note that building a dependent which is not a build2 module in
            // the same configuration with the build2 module it depends upon
            // is an error.
            //
            if (buildtime           &&
                !build2_module (nm) &&
                build2_module (dn)  &&
                pdb == *ddb)
            {
              assert (dr == nullptr); // Should fail on the "silent" run.

              // Note that the dependent package information is printed by the
              // above exception guard.
              //
              fail << "unable to build build system module " << dn
                   << " in its dependent package configuration "
                   << pdb.config_orig <<
                info << "use --config-* to select suitable configuration";
            }

            // If we didn't get the available package corresponding to the
            // selected package, look for any that satisfies the constraint.
            //
            if (dap == nullptr)
            {
              // And if we have no repository fragment to look in, then that
              // means the package is an orphan (we delay this check until we
              // actually need the repository fragment to allow orphans
              // without prerequisites).
              //
              if (af == nullptr)
              {
                assert (dr == nullptr); // Should fail on the "silent" run.

                fail << "package " << pkg.available_name_version_db ()
                     << " is orphaned" <<
                  info << "explicitly upgrade it to a new version";
              }

              // We look for prerequisites only in the repositories of this
              // package (and not in all the repositories of this
              // configuration). At first this might look strange, but it also
              // kind of makes sense: we only use repositories "approved" for
              // this package version. Consider this scenario as an example:
              // hello/1.0.0 and libhello/1.0.0 in stable and libhello/2.0.0
              // in testing. As a prerequisite of hello, which version should
              // libhello resolve to? While one can probably argue either way,
              // resolving it to 1.0.0 is the conservative choice and the user
              // can always override it by explicitly building libhello.
              //
              // Note though, that if this is a test package, then its special
              // test dependencies (main packages that refer to it) should be
              // searched upstream through the complement repositories
              // recursively, since the test packages may only belong to the
              // main package's repository and its complements.
              //
              // @@ Currently we don't implement the reverse direction search
              //    for the test dependencies, effectively only supporting the
              //    common case where the main and test packages belong to the
              //    same repository. Will need to fix this eventually.
              //
              // Note that this logic (naturally) does not apply if the
              // package is already selected by the user (see above).
              //
              // Also note that for the user-specified dependency version
              // constraint we rely on the satisfying package version be
              // present in repositories of the first dependent met. As a
              // result, we may fail too early if such package version doesn't
              // belong to its repositories, but belongs to the ones of some
              // dependent that we haven't met yet. Can we just search all
              // repositories for an available package of the appropriate
              // version and just take it, if present? We could, but then
              // which repository should we pick? The wrong choice can
              // introduce some unwanted repositories and package versions
              // into play. So instead, we will postpone collecting the
              // problematic dependent, expecting that some other one will
              // find the appropriate version in its repositories.
              //
              // For a system package we will try to find the available
              // package that matches the constraint (preferable for the
              // configuration negotiation machinery) and, if fail, fallback
              // to picking the latest one just to make sure the package is
              // recognized. An unrecognized package means the broken/stale
              // repository (see below).
              //
              rp = find_available_one (dn, d.constraint, af);

              if (dap == nullptr && system && d.constraint)
                rp = find_available_one (dn, nullopt, af);

              if (dap == nullptr)
              {
                if (dep_constr && !system && postponed_repo != nullptr)
                {
                  // We shouldn't be called in the diag mode for the postponed
                  // package builds.
                  //
                  assert (dr == nullptr);

                  if (!dry_run)
                  {
                    l5 ([&]{trace << "rep-postpone dependent "
                                  << pkg.available_name_version_db ()
                                  << " due to dependency " << dp
                                  << " and user-specified constraint "
                                  << *dep_constr;});

                    postponed_repo->insert (&pkg);
                  }

                  return precollect_result (true /* postpone */);
                }

                // Fail if we are unable to find an available dependency
                // package which satisfies the dependent's constraint.
                //
                // It feels that just considering this alternative as
                // unsatisfactory and silently trying another alternative
                // would be wrong, since the user may rather want to
                // fix/re-fetch the repository and retry.
                //
                diag_record dr (fail);

                // Issue diagnostics differently based on the presence of
                // available packages for the unsatisfied dependency.
                //
                // Note that there can't be any stubs, since they satisfy
                // any constraint and we won't be here if there were any.
                //
                vector<shared_ptr<available_package>> aps (
                  find_available (dn, nullopt /* version_constraint */, af));

                if (!aps.empty ())
                {
                  dr << "unable to satisfy dependency constraint (" << dn;

                  // We need to be careful not to print the wildcard-based
                  // constraint.
                  //
                  if (d.constraint &&
                      (!dep_constr || !wildcard (*dep_constr)))
                    dr << ' ' << *d.constraint;

                  dr << ") of package " << nm << pdb <<
                    info << "available " << dn << " versions:";

                  for (const shared_ptr<available_package>& ap: aps)
                    dr << ' ' << ap->version;
                }
                else
                {
                  dr << "no package available for dependency " << dn
                     << " of package " << nm << pdb;
                }

                // Avoid printing this if the dependent package is external
                // since it's more often confusing than helpful (they are
                // normally not fetched manually).
                //
                if (!af->location.empty ()           &&
                    !af->location.directory_based () &&
                    (!dep_constr || system))
                  dr << info << "repository " << af->location << " appears "
                     << "to be broken" <<
                    info << "or the repository state could be stale" <<
                    info << "run 'bpkg rep-fetch' to update";
              }

              // If all that's available is a stub then we need to make sure
              // the package is present in the system repository and it's
              // version satisfies the constraint. If a source package is
              // available but there is a system package specified on the
              // command line and it's version satisfies the constraint then
              // the system package should be preferred. To recognize such a
              // case we just need to check if the authoritative system
              // version is set and it satisfies the constraint. If the
              // corresponding system package is non-optional it will be
              // preferred anyway.
              //
              if (dap->stub ())
              {
                // Note that the constraint can safely be printed as it can't
                // be a wildcard (produced from the user-specified dependency
                // version constraint). If it were, then the system version
                // wouldn't be NULL and would satisfy itself.
                //
                if (dap->system_version (*ddb) == nullptr)
                {
                  if (dr != nullptr)
                    *dr << error << "dependency " << d << " of package "
                        << nm << " is not available in source" <<
                      info << "specify ?sys:" << dn << " if it is available "
                        << "from the system";

                  return precollect_result (false /* postpone */);
                }

                if (!satisfies (*dap->system_version (*ddb), d.constraint))
                {
                  if (dr != nullptr)
                    *dr << error << "dependency " << d << " of package "
                        << nm << " is not available in source" <<
                      info << package_string (dn,
                                              *dap->system_version (*ddb),
                                              true /* system */)
                        << " does not satisfy the constrains";

                  return precollect_result (false /* postpone */);
                }

                system = true;
              }
              else
              {
                auto p (dap->system_version_authoritative (*ddb));

                if (p.first != nullptr &&
                    p.second && // Authoritative.
                    satisfies (*p.first, d.constraint))
                  system = true;
              }
            }

            bool ru (i != map_.end () || dsp != nullptr);

            if (!ru)
              reused = false;

            r.push_back (prebuild {d,
                  *ddb,
                  move (dsp),
                  move (dap),
                  move (rp.second),
                  system,
                  specified,
                  force,
                  ru});
          }

          // Now, as we have pre-collected the dependency builds, go through
          // them and check that for those dependencies which are already
          // being built we will be able to choose one of them (either
          // existing or new) which satisfies all the dependents. If that's
          // not the case, then issue the diagnostics, if requested, and
          // return the unsatisfactory dependency builds.
          //
          // Note that collect_build() also performs this check but postponing
          // it till then can end up in failing instead of selecting some
          // other dependency alternative.
          //
          for (const prebuild& b: r)
          {
            const shared_ptr<available_package>& dap (b.available);

            assert (dap != nullptr); // Otherwise we would fail earlier.

            const dependency& d (b.dependency);

            auto i (map_.find (b.db, d.name));

            if (i != map_.end () && d.constraint)
            {
              const build_package& bp (i->second.package);

              if (bp.action && *bp.action == build_package::build)
              {
                const version& v1 (b.system
                                   ? *dap->system_version (b.db)
                                   : dap->version);

                const version& v2 (bp.available_version ());

                if (v1 != v2)
                {
                  using constraint_type = build_package::constraint_type;

                  constraint_type c1 {pdb, nm.string (), *d.constraint};

                  if (!satisfies (v2, c1.value))
                  {
                    for (const constraint_type& c2: bp.constraints)
                    {
                      if (!satisfies (v1, c2.value))
                      {
                        if (dr != nullptr)
                        {
                          const package_name& n  (d.name);
                          const string&       d1 (c1.dependent);
                          const string&       d2 (c2.dependent);

                          *dr << error << "unable to satisfy constraints on "
                              << "package " << n <<
                            info << d2 << c2.db << " depends on (" << n << ' '
                                 << c2.value << ")" <<
                            info << d1 << c1.db << " depends on (" << n << ' '
                                 << c1.value << ")" <<
                            info << "available "
                                 << bp.available_name_version () <<
                            info << "available "
                                 << package_string (n, v1, b.system) <<
                            info << "explicitly specify " << n << " version "
                                 << "to manually satisfy both constraints";
                        }

                        return precollect_result (reused, move (r));
                      }
                    }
                  }
                }
              }
            }
          }

          return precollect_result (move (r), reused);
        };

      // Try to collect the previously collected pre-builds.
      //
      // Return false if the dependent has configuration clauses and is
      // postponed until dependencies configuration negotiation.
      //
      auto collect = [&options,
                      &pkg,
                      &pdb,
                      &nm,
                      &pk,
                      &fdb,
                      &rpt_depts,
                      &apc,
                      initial_collection,
                      &replaced_vers,
                      &dep_chain,
                      postponed_repo,
                      postponed_alts,
                      &postponed_deps,
                      &postponed_cfgs,
                      &postponed_poss,
                      &unacceptable_alts,
                      &di,
                      reeval,
                      &reeval_pos,
                      &reevaluated,
                      &fail_reeval,
                      &edas,
                      &das,
                      &precollect,
                      &trace,
                      this]
        (const dependency_alternative& da,
         size_t dai,
         prebuilds&& bs,
         const package_prerequisites* prereqs)
        {
          // Dependency alternative position.
          //
          pair<size_t, size_t> dp (di + 1, dai + 1);

          if (reeval                        &&
              dp.first  == reeval_pos.first &&
              dp.second != reeval_pos.second)
            fail_reeval ();

          postponed_configuration::packages cfg_deps;

          for (prebuild& b: bs)
          {
            build_package bp {
              build_package::build,
                b.db,
                b.selected,
                b.available,
                move (b.repository_fragment),
                nullopt,                    // Dependencies.
                nullopt,                    // Dependencies alternatives.
                nullopt,                    // Package skeleton.
                nullopt,                    // Postponed dependency alternatives.
                false,                      // Recursive collection.
                nullopt,                    // Hold package.
                nullopt,                    // Hold version.
                {},                         // Constraints.
                b.system,
                false,                      // Keep output directory.
                false,                      // Disfigure (from-scratch reconf).
                false,                      // Configure-only.
                nullopt,                    // Checkout root.
                false,                      // Checkout purge.
                strings (),                 // Configuration variables.
                {pk},                       // Required by (dependent).
                true,                       // Required by dependents.
                0};                         // State flags.

            const optional<version_constraint>& constraint (
              b.dependency.constraint);

            // Add our constraint, if we have one.
            //
            // Note that we always add the constraint implied by the
            // dependent.  The user-implied constraint, if present, will be
            // added when merging from the pre-entered entry. So we will have
            // both constraints for completeness.
            //
            if (constraint)
              bp.constraints.emplace_back (pdb, nm.string (), *constraint);

            // Now collect this prerequisite. If it was actually collected
            // (i.e., it wasn't already there) and we are forcing a downgrade
            // or upgrade, then refuse for a held version, warn for a held
            // package, and print the info message otherwise, unless the
            // verbosity level is less than two.
            //
            // Note though that while the prerequisite was collected it could
            // have happen because it is an optional package and so not being
            // pre-collected earlier. Meanwhile the package was specified
            // explicitly and we shouldn't consider that as a
            // dependency-driven up/down-grade enforcement.
            //
            // Here is an example of the situation we need to handle properly:
            //
            // repo: foo/2(->bar/2), bar/0+1
            // build sys:bar/1
            // build foo ?sys:bar/2
            //
            // Pass the function which verifies we don't try to force
            // up/downgrade of the held version and makes sure we don't print
            // the dependency chain if replace_version will be thrown.
            //
            // Also note that we rely on "small function object" optimization
            // here.
            //
            struct
            {
              const build_package& dependent;
              const prebuild&      prerequisite;
            } dpn {pkg, b};

            const function<verify_package_build_function> verify (
              [&dpn, &dep_chain] (const build_package& p, bool scratch)
              {
                const prebuild&      prq (dpn.prerequisite);
                const build_package& dep (dpn.dependent);

                if (prq.force && !prq.specified_dependency)
                {
                  // Fail if the version is held. Otherwise, warn if the
                  // package is held.
                  //
                  bool f (prq.selected->hold_version);
                  bool w (!f && prq.selected->hold_package);

                  // Note that there is no sense to warn or inform the user if
                  // we are about to start re-collection from scratch.
                  //
                  // @@ It seems that we may still warn/inform multiple times
                  //    about the same package if we start from scratch. The
                  //    intermediate diagnostics can probably be irrelevant to
                  //    the final result.
                  //
                  //    Perhaps what we should do is queue the diagnostics and
                  //    then, if the run is not scratched, issues it. And if
                  //    it is scratched, then drop it.
                  //
                  if (f || ((w || verb >= 2) && !scratch))
                  {
                    const version& av (p.available_version ());

                    bool u (av > prq.selected->version);
                    bool c (prq.dependency.constraint);

                    diag_record dr;

                    (f ? dr << fail :
                     w ? dr << warn :
                     dr << info)
                      << "package " << dep.name () << dep.db
                      << " dependency on " << (c ? "(" : "") << prq.dependency
                      << (c ? ")" : "") << " is forcing "
                      << (u ? "up" : "down") << "grade of " << *prq.selected
                      << prq.db << " to ";

                    // Print both (old and new) package names in full if the
                    // system attribution changes.
                    //
                    if (prq.selected->system ())
                      dr << p.available_name_version ();
                    else
                      dr << av; // Can't be a system version so is never wildcard.

                    if (prq.selected->hold_version)
                      dr << info << "package version " << *prq.selected
                         << prq.db<< " is held";

                    if (f)
                      dr << info << "explicitly request version "
                         << (u ? "up" : "down") << "grade to continue";
                  }
                }

                // Don't print the "while satisfying..." chain if we are about
                // to re-collect the packages.
                //
                if (scratch)
                  dep_chain.clear ();
              });

            // Note: non-recursive.
            //
            build_package* p (
              collect_build (options,
                             move (bp),
                             fdb,
                             rpt_depts,
                             apc,
                             initial_collection,
                             replaced_vers,
                             postponed_cfgs,
                             nullptr /* dep_chain */,
                             nullptr /* postponed_repo */,
                             nullptr /* postponed_alts */,
                             nullptr /* postponed_deps */,
                             nullptr /* postponed_poss */,
                             nullptr /* unacceptable_alts */,
                             verify));

            package_key dpk (b.db, b.available->id.name);

            // Do not collect prerequisites recursively for dependent
            // re-evaluation. Instead, if the re-evaluation position is
            // reached, collect the dependency packages to add them to the
            // existing dependent's cluster.
            //
            if (reeval)
            {
              if (dp == reeval_pos)
                cfg_deps.push_back (move (dpk));

              continue;
            }

            // Do not recursively collect a dependency of a dependent with
            // configuration clauses, which could be this or some other
            // (indicated by the presence in postponed_deps) dependent. In the
            // former case if the prerequisites were prematurely collected,
            // throw postpone_dependency.
            //
            // Note that such a dependency will be recursively collected
            // directly right after the configuration negotiation (rather than
            // via the dependent).
            //
            bool collect_prereqs (p != nullptr);

            {
              build_package* bp (entered_build (dpk));
              assert (bp != nullptr);

              if (da.prefer || da.require)
              {
                // Indicate that the dependent with configuration clauses is
                // present.
                //
                {
                  auto i (postponed_deps.find (dpk));

                  // Do not override postponements recorded during postponed
                  // collection phase with those recorded during initial
                  // phase.
                  //
                  if (i == postponed_deps.end ())
                  {
                    postponed_deps.emplace (dpk,
                                            postponed_dependency {
                                              false /* without_config */,
                                                true  /* with_config */,
                                                initial_collection});
                  }
                  else
                    i->second.with_config = true;
                }

                // Prematurely collected before we saw any config clauses.
                //
                if (bp->recursive_collection &&
                    postponed_cfgs.find_dependency (dpk) == nullptr)
                {
                  l5 ([&]{trace << "cannot cfg-postpone dependency "
                                << bp->available_name_version_db ()
                                << " of dependent "
                                << pkg.available_name_version_db ()
                                << " (collected prematurely), "
                                << "throwing postpone_dependency";});

                  // Don't print the "while satisfying..." chain.
                  //
                  dep_chain.clear ();

                  throw postpone_dependency (move (dpk));
                }

                // Postpone until (re-)negotiation.
                //
                l5 ([&]{trace << "cfg-postpone dependency "
                              << bp->available_name_version_db ()
                              << " of dependent "
                              << pkg.available_name_version_db ();});

                cfg_deps.push_back (move (dpk));

                collect_prereqs = false;
              }
              else
              {
                // Indicate that the dependent without configuration clauses
                // is also present.
                //
                auto i (postponed_deps.find (dpk));
                if (i != postponed_deps.end ())
                {
                  l5 ([&]{trace << "dep-postpone dependency "
                                << bp->available_name_version_db ()
                                << " of dependent "
                                << pkg.available_name_version_db ();});

                  i->second.wout_config = true;

                  collect_prereqs = false;
                }
                else
                {
                  l5 ([&]{trace << "no cfg-clause for dependency "
                                << bp->available_name_version_db ()
                                << " of dependent "
                                << pkg.available_name_version_db ();});
                }
              }
            }

            if (collect_prereqs)
              collect_build_prerequisites (options,
                                           *p,
                                           fdb,
                                           rpt_depts,
                                           apc,
                                           initial_collection,
                                           replaced_vers,
                                           dep_chain,
                                           postponed_repo,
                                           postponed_alts,
                                           0 /* max_alt_index */,
                                           postponed_deps,
                                           postponed_cfgs,
                                           postponed_poss,
                                           unacceptable_alts);
          }

          // If this dependent has any dependencies with configurations
          // clauses, then we need to deal with that.
          //
          // This is what we refer to as the "up-negotiation" where we
          // negotiate the configuration of dependents that could not be
          // postponed and handled all at once during "initial negotiation" in
          // collect_build_postponed().
          //
          if (!cfg_deps.empty ())
          {
            // First, determine if there is any unprocessed reused dependency
            // alternative that we can potentially use instead of the current
            // one if it turns out that a configuration for some of its
            // dependencies cannot be negotiated between all the dependents
            // (see unacceptable_alternatives for details).
            //
            bool has_alt (false);
            {
              // Find the index of the current dependency alternative.
              //
              size_t i (0);
              for (; i != edas.size (); ++i)
              {
                if (&edas[i].first.get () == &da)
                  break;
              }

              // The current dependency alternative must be present in the
              // list.
              //
              assert (i != edas.size ());

              // Return true if the current alternative is unacceptable.
              //
              auto unacceptable =
                [&pk, &pkg, di, &i, &edas, &unacceptable_alts] ()
                {
                  // Convert to 1-base.
                  //
                  pair<size_t, size_t> pos (di + 1, edas[i].second + 1);

                  return unacceptable_alts.find (
                    unacceptable_alternative (pk,
                                              pkg.available->version,
                                              pos)) !=
                  unacceptable_alts.end ();
                };

              // See if there is any unprocessed reused alternative to the
              // right.
              //
              // Note that this is parallel to the alternative selection
              // logic.
              //
              for (++i; i != edas.size (); ++i)
              {
                if (unacceptable ())
                  continue;

                const dependency_alternative& a (edas[i].first);

                precollect_result r (precollect (a,
                                                 das.buildtime,
                                                 prereqs,
                                                 nullptr /* diag_record */,
                                                 true /* dru_run */));

                if (r.builds && r.reused)
                {
                  has_alt = true;
                  break;
                }
              }

              // If there are none and we are in the "recreate dependency
              // decisions" mode, then repeat the search in the "make
              // dependency decisions" mode.
              //
              if (!has_alt && prereqs != nullptr)
              {
                for (i = 0; i != edas.size (); ++i)
                {
                  if (unacceptable ())
                    continue;

                  const dependency_alternative& a (edas[i].first);

                  if (&a != &da) // Skip the current dependency alternative.
                  {
                    precollect_result r (precollect (a,
                                                     das.buildtime,
                                                     nullptr /* prereqs */,
                                                     nullptr /* diag_record */,
                                                     true /* dru_run */));

                    if (r.builds && r.reused)
                    {
                      has_alt = true;
                      break;
                    }
                  }
                }
              }
            }

            // Re-evaluation is a special case (it happens during cluster
            // negotiation; see collect_build_postponed()).
            //
            if (reeval)
            {
              reevaluated = true;

              // Note: the dependent may already exist in the cluster with a
              // subset of dependencies.
              //
              postponed_configuration& cfg (
                postponed_cfgs.add (pk,
                                    true /* existing */,
                                    dp,
                                    cfg_deps,
                                    has_alt).first);

              // Can we merge clusters as a result? Seems so.
              //
              // - Simple case is if the cluster(s) being merged are not
              //   negotiated. Then perhaps we could handle this via the same
              //   logic that handles the addition of extra dependencies.
              //
              // - For the complex case, perhaps just making the resulting
              //   cluster shadow and rolling back, just like in the other
              //   case (non-existing dependent).
              //
              // Note: this is a special case of the below more general logic.
              //
              // Also note that we can distinguish the simple case by the fact
              // that the resulting cluster is not negotiated. Note however,
              // that in this case it is guaranteed that all the involved
              // clusters will be merged into the cluster which the being
              // re-evaluated dependent belongs to since this cluster (while
              // not being negotiated) already has non-zero depth (see
              // collect_build_postponed() for details).
              //
              assert (cfg.depth != 0);

              if (cfg.negotiated)
              {
                l5 ([&]{trace << "re-evaluating dependent "
                              << pkg.available_name_version_db ()
                              << " involves negotiated configurations and "
                              << "results in " << cfg << ", throwing "
                              << "merge_configuration";});

                // Don't print the "while satisfying..." chain.
                //
                dep_chain.clear ();

                throw merge_configuration {cfg.depth};
              }

              l5 ([&]{trace << "re-evaluating dependent "
                            << pkg.available_name_version_db ()
                            << " results in " << cfg;});

              return false;
            }

            // As a first step add this dependent/dependencies to one of the
            // new/existing postponed_configuration clusters, which could
            // potentially cause some of them to be merged. Here are the
            // possibilities and what we should do in each case.
            //
            // 1. Got added to a new cluster -- this dependent got postponed
            //    and we return false.
            //
            // 2. Got added to an existing non-yet-negotiated cluster (which
            //    could potentially involve merging a bunch of them) -- ditto.
            //
            // 3. Got added to an existing already-[being]-negotiated cluster
            //    (which could potentially involve merging a bunch of them,
            //    some negotiated, some being negotiated, and some not yet
            //    negotiated) -- see below logic.
            //
            // Note that if a dependent is postponed, it will be recursively
            // recollected right after the configuration negotiation.

            // Note: don't move the argument from since may be needed for
            // constructing exception.
            //
            pair<postponed_configuration&, optional<bool>> r (
              postponed_cfgs.add (pk,
                                  false /* existing */,
                                  dp,
                                  cfg_deps,
                                  has_alt));

            postponed_configuration& cfg (r.first);

            if (cfg.depth == 0)
              return false; // Cases (1) or (2).
            else
            {
              // Case (3).
              //
              // There is just one complication:
              //
              // If all the merged clusters are already negotiated, then all
              // is good: all the dependencies in cfg_deps have been collected
              // recursively as part of the configuration negotiation (because
              // everything in this cluster is already negotiated) and we can
              // return true (no need to postpone any further steps).
              //
              // But if we merged clusters not yet negotiated, or, worse,
              // being in the middle of negotiation, then we need to get this
              // merged cluster into the fully negotiated state. The way we do
              // it is by throwing merge_configuration (see below).
              //
              // When we are back here after throwing merge_configuration,
              // then all the clusters have been pre-merged and our call to
              // add() shouldn't have added any new cluster. In this case the
              // cluster can either be already negotiated or being negotiated
              // and we can proceed as in the "everything is negotiated case"
              // above (we just need to get the the dependencies that we care
              // about into the recursively collected state).
              //

              // To recap, r.second values mean:
              //
              //   absent  -- shadow cluster-based merge is/being negotiated
              //   false   -- some non or being negotiated
              //   true    -- all have been negotiated
              //
              if (r.second && !*r.second)
              {
                // The partially negotiated case.
                //
                // Handling this in a straightforward way is not easy due to
                // the being negotiated cases -- we have code up the stack
                // that is in the middle of the negotiation logic.
                //
                // Another idea is to again throw to the outer try/catch frame
                // (thus unwinding all the being negotiated code) and complete
                // the work there. The problem with this approach is that
                // without restoring the state we may end up with unrelated
                // clusters that will have no corresponding try-catch frames
                // (because we may unwind them in the process).
                //
                // So the approach we will use is the "shadow" idea for
                // merging clusters. Specifically, we throw
                // merge_configuration to the outer try/catch. At the catch
                // site we make the newly merged cluster a shadow of the
                // restored cluster and retry the same steps similar to
                // retry_configuration. As we redo these steps, we consult the
                // shadow cluster and if the dependent/dependency entry is
                // there, then instead of adding it to another (new/existing)
                // cluster that would later be merged into this non-shadow
                // cluster, we add it directly to the non-shadow cluster
                // (potentially merging other cluster which it feels like by
                // definition should all be already fully negotiated). The end
                // result is that once we reach this point again, there will
                // be nothing to merge.
                //
                // The shadow check is part of postponed_configs::add().
                //
                l5 ([&]{trace << "cfg-postponing dependent "
                              << pkg.available_name_version_db ()
                              << " merges non-negotiated and/or being "
                              << "negotiated configurations in and results in "
                              << cfg << ", throwing merge_configuration";});

                // Don't print the "while satisfying..." chain.
                //
                dep_chain.clear ();

                throw merge_configuration {cfg.depth};
              }

              // Up-negotiate the configuration and if it has changed, throw
              // retry_configuration to the try/catch frame corresponding to
              // the negotiation of the outermost merged cluster in order to
              // retry the same steps (potentially refining the configuration
              // as we go along) and likely (but not necessarily) ending up
              // here again, at which point we up-negotiate again with the
              // expectation that the configuration won't change (but if it
              // does, then we throw again and do another refinement pass).
              //
              // In a sense, semantically, we should act like a one more
              // iteration of the initial negotiation loop with the exception
              // acting like a request to restart the refinement process from
              // the beginning.
              //
              bool changed;
              {
                // Similar to initial negotiation, resolve package skeletons
                // for this dependent and its dependencies.
                //
                assert (pkg.skeleton);
                package_skeleton& dept (*pkg.skeleton);

                // If a dependency has already been recursively collected,
                // then we can no longer call reload_defaults() or
                // verify_sensible() on its skeleton. We could reset it, but
                // then we wouldn't be able to continue using it if
                // negotiate_configuration() below returns false. So it seems
                // the most sensible approach is to make a temporary copy and
                // reset that.
                //
                small_vector<reference_wrapper<package_skeleton>, 1> depcs;
                forward_list<package_skeleton> depcs_storage; // Ref stability.
                {
                  depcs.reserve (cfg_deps.size ());
                  for (const package_key& pk: cfg_deps)
                  {
                    build_package* b (entered_build (pk));
                    assert (b != nullptr);

                    package_skeleton* depc;
                    if (b->recursive_collection)
                    {
                      assert (b->skeleton);

                      depcs_storage.push_front (*b->skeleton);
                      depc = &depcs_storage.front ();
                      depc->reset ();
                    }
                    else
                      depc = &(b->skeleton
                               ? *b->skeleton
                               : b->init_skeleton (options));

                    depcs.push_back (*depc);
                  }
                }

                optional<bool> c (
                  negotiate_configuration (
                    cfg.dependency_configurations, dept, dp, depcs, has_alt));

                // If the dependency alternative configuration cannot be
                // negotiated for this dependent, then add an entry to
                // unacceptable_alts and throw unaccept_alternative to
                // recollect from scratch.
                //
                if (!c)
                {
                  unacceptable_alts.emplace (pk, pkg.available->version, dp);

                  l5 ([&]{trace << "unable to cfg-negotiate dependency "
                                << "alternative " << dp.first << ','
                                << dp.second << " for dependent "
                                << pkg.available_name_version_db ()
                                << ", throwing unaccept_alternative";});

                  // Don't print the "while satisfying..." chain.
                  //
                  dep_chain.clear ();

                  throw unaccept_alternative ();
                }
                else
                  changed = *c;
              }

              // If the configuration hasn't changed, then we carry on.
              // Otherwise, retry the negotiation from the beginning to refine
              // the resulting configuration (see the catch block for
              // retry_configuration).
              //
              if (changed)
              {
                l5 ([&]{trace << "cfg-postponing dependent "
                              << pkg.available_name_version_db ()
                              << " involves (being) negotiated configurations "
                              << "and results in " << cfg
                              << ", throwing retry_configuration";});

                // Don't print the "while satisfying..." chain.
                //
                dep_chain.clear ();

                throw retry_configuration {cfg.depth, move (pk)};
              }

              l5 ([&]{trace << "configuration for cfg-postponed "
                            << "dependencies of dependent "
                            << pkg.available_name_version_db () << " is "
                            << (r.second ? "" : "shadow-") << "negotiated";});

              // Note that even in the fully negotiated case we may still add
              // extra dependencies to this cluster which we still need to
              // configure and recursively collect before indicating to the
              // caller (returning true) that we are done with this depends
              // value and the dependent is not postponed.
              //
              for (const package_key& p: cfg_deps)
              {
                build_package* b (entered_build (p));
                assert (b != nullptr);

                // Reconfigure the configured dependencies (see
                // collect_build_postponed() for details).
                //
                if (b->selected != nullptr &&
                    b->selected->state == package_state::configured)
                  b->flags |= build_package::adjust_reconfigure;

                if (!b->recursive_collection)
                {
                  l5 ([&]{trace << "collecting cfg-postponed dependency "
                                << b->available_name_version_db ()
                                << " of dependent "
                                << pkg.available_name_version_db ();});

                  // Similar to the inital negotiation case, verify and set
                  // the dependent configuration for this dependency.
                  //
                  {
                    assert (b->skeleton); // Should have been init'ed above.

                    const package_configuration& pc (
                      cfg.dependency_configurations[p]);

                    pair<bool, string> pr (b->skeleton->available != nullptr
                                           ? b->skeleton->verify_sensible (pc)
                                           : make_pair (true, string ()));

                    if (!pr.first)
                    {
                      diag_record dr (fail);
                      dr << "unable to negotiate sensible configuration for "
                         << "dependency " << p << '\n'
                         << "  " << pr.second;

                      dr << info << "negotiated configuration:\n";
                      pc.print (dr, "    ");
                    }

                    b->skeleton->dependent_config (pc);
                  }

                  collect_build_prerequisites (options,
                                               *b,
                                               fdb,
                                               rpt_depts,
                                               apc,
                                               initial_collection,
                                               replaced_vers,
                                               dep_chain,
                                               postponed_repo,
                                               postponed_alts,
                                               0 /* max_alt_index */,
                                               postponed_deps,
                                               postponed_cfgs,
                                               postponed_poss,
                                               unacceptable_alts);
                }
                else
                  l5 ([&]{trace << "dependency "
                                << b->available_name_version_db ()
                                << " of dependent "
                                << pkg.available_name_version_db ()
                                << " is already (being) recursively "
                                << "collected, skipping";});
              }

              return true;
            }
          }

          return true;
        };

      // Select a dependency alternative, copying it alone into the resulting
      // dependencies list and evaluating its reflect clause, if present.
      //
      bool selected (false);
      auto select = [&sdeps, &salts, &sdas, &skel, di, &selected]
        (const dependency_alternative& da, size_t dai)
        {
          assert (sdas.empty ());

          // Avoid copying enable/reflect not to evaluate them repeatedly.
          //
          sdas.emplace_back (nullopt /* enable */,
                             nullopt /* reflect */,
                             da.prefer,
                             da.accept,
                             da.require,
                             da /* dependencies */);

          sdeps.push_back (move (sdas));
          salts.push_back (dai);

          if (da.reflect)
            skel.evaluate_reflect (*da.reflect, make_pair (di, dai));

          selected = true;
        };

      // Postpone the prerequisite builds collection, optionally inserting the
      // package to the postponements set (can potentially already be there)
      // and saving the enabled alternatives.
      //
      auto postpone = [&pkg, &edas, &postponed]
        (postponed_packages* postpones)
        {
          if (postpones != nullptr)
            postpones->insert (&pkg);

          pkg.postponed_dependency_alternatives = move (edas);
          postponed = true;
        };

      // Iterate over the enabled dependencies and try to select a
      // satisfactory alternative.
      //
      // If the package is already configured as source and is not
      // up/downgraded, then we will try to resolve its dependencies to the
      // current prerequisites. To achieve this we will first try to select an
      // alternative in the "recreate dependency decisions" mode, filtering
      // out all the alternatives where dependencies do not all belong to the
      // list of current prerequisites. If we end up with no alternative
      // selected, then we retry in the "make dependency decisions" mode and
      // select the alternative ignoring the current prerequisites.
      //
      // Note though, that if we are re-evaluating an existing dependent
      // then we fail if we didn't succeed in the "recreate dependency
      // decisions" mode.
      //
      const package_prerequisites* prereqs (src_conf && !ud
                                            ? &sp->prerequisites
                                            : nullptr);

      // During the dependent re-evaluation we always try to reproduce the
      // existing setup.
      //
      assert (!reeval || prereqs != nullptr);

      for (bool unacceptable (false);;)
      {
        // The index and pre-collection result of the first satisfactory
        // alternative.
        //
        optional<pair<size_t, precollect_result>> first_alt;

        // The number of satisfactory alternatives.
        //
        size_t alts_num (0);

        // If true, then only reused alternatives will be considered for the
        // selection.
        //
        // The idea here is that we don't want to bloat the configuration by
        // silently configuring a new dependency package as the alternative
        // for an already used but not satisfactory for all the dependents
        // dependency. Think of silently configuring Qt6 just because the
        // configured version of Qt5 is not satisfactory for all the
        // dependents. The user must have a choice if to either configure this
        // new dependency by specifying it explicitly or, for example, to
        // upgrade dependents so that the existing dependency is satisfactory
        // for all of them.
        //
        // Note that if there are multiple alternatives with all their
        // dependencies resolved/satisfied, then only reused alternatives are
        // considered anyway. Thus, this flag only affects the single
        // alternative case.
        //
        bool reused_only (false);

        for (size_t i (0); i != edas.size (); ++i)
        {
          // Skip the unacceptable alternatives.
          //
          {
            // Convert to 1-base.
            //
            pair<size_t, size_t> pos (di + 1, edas[i].second + 1);

            if (unacceptable_alts.find (
                  unacceptable_alternative (pk, ap->version, pos)) !=
                unacceptable_alts.end ())
            {
              unacceptable = true;

              l5 ([&]{trace << "dependency alternative " << pos.first << ','
                            << pos.second << " for dependent "
                            << pkg.available_name_version_db ()
                            << " is unacceptable, skipping";});

              continue;
            }
          }

          const dependency_alternative& da (edas[i].first);

          precollect_result r (precollect (da, das.buildtime, prereqs));

          // If we didn't come up with satisfactory dependency builds, then
          // skip this alternative and try the next one, unless the collecting
          // is postponed in which case just bail out.
          //
          // Should we skip alternatives for which we are unable to satisfy
          // the constraint? On one hand, this could be a user error: there is
          // no package available from dependent's repositories that satisfies
          // the constraint. On the other hand, it could be that it's other
          // dependent's constraints that we cannot satisfy together with
          // others. And in this case we may want some other
          // alternative. Consider, as an example, something like this:
          //
          // depends: libfoo >= 2.0.0 | {libfoo >= 1.0.0 libbar}
          //
          if (!r.builds)
          {
            if (r.repo_postpone)
            {
              if (reeval)
                fail_reeval ();

              postpone (nullptr); // Already inserted into postponed_repo.
              break;
            }

            // If this alternative is reused but is not satisfactory, then
            // switch to the reused-only mode.
            //
            if (r.reused && r.unsatisfactory)
              reused_only = true;

            continue;
          }

          ++alts_num;

          // Note that when we see the first satisfactory alternative, we
          // don't know yet if it is a single alternative or the first of the
          // (multiple) true alternatives (those are handled differently).
          // Thus, we postpone its processing until the second satisfactory
          // alternative is encountered or the end of the alternatives list is
          // reached.
          //
          if (!first_alt)
          {
            first_alt = make_pair (i, move (r));
            continue;
          }

          // Try to collect and then select a true alternative, returning true
          // if the alternative is selected or the collection is postponed.
          // Return false if the alternative is ignored (not postponed and not
          // all of it dependencies are reused).
          //
          auto try_select = [postponed_alts, &max_alt_index,
                             &edas, &pkg,
                             prereqs,
                             reeval,
                             &trace,
                             &postpone, &collect, &select]
                            (size_t index, precollect_result&& r)
          {
            const auto& eda (edas[index]);
            const dependency_alternative& da (eda.first);
            size_t dai (eda.second);

            // Postpone the collection if the alternatives maximum index is
            // reached.
            //
            if (postponed_alts != nullptr && index >= max_alt_index)
            {
              // For a dependent re-evaluation max_alt_index is expected to be
              // max size_t.
              //
              assert (!reeval);

              l5 ([&]{trace << "alt-postpone dependent "
                            << pkg.available_name_version_db ()
                            << " since max index is reached: " << index <<
                    info << "dependency alternative: " << da;});

              postpone (postponed_alts);
              return true;
            }

            // Select this alternative if all its dependencies are reused and
            // do nothing about it otherwise.
            //
            if (r.reused)
            {
              // On the diagnostics run there shouldn't be any alternatives
              // that we could potentially select.
              //
              assert (postponed_alts != nullptr);

              if (!collect (da, dai, move (*r.builds), prereqs))
              {
                postpone (nullptr); // Already inserted into postponed_cfgs.
                return true;
              }

              select (da, dai);

              // Make sure no more true alternatives are selected during this
              // function call unless we are re-evaluating a dependent.
              //
              if (!reeval)
                max_alt_index = 0;

              return true;
            }
            else
              return false;
          };

          // If we encountered the second satisfactory alternative, then this
          // is the "multiple true alternatives" case. In this case we also
          // need to process the first satisfactory alternative, which
          // processing was delayed.
          //
          if (alts_num == 2)
          {
            assert (first_alt);

            if (try_select (first_alt->first, move (first_alt->second)))
              break;
          }

          if (try_select (i, move (r)))
            break;

          // Not all of the alternative dependencies are reused, so go to
          // the next alternative.
        }

        // Bail out if the collection is postponed for any reason.
        //
        if (postponed)
          break;

        // Select the single satisfactory alternative if it is reused or we
        // are not in the reused-only mode.
        //
        if (!selected && alts_num == 1)
        {
          assert (first_alt);

          precollect_result& r (first_alt->second);

          assert (r.builds);

          if (r.reused || !reused_only)
          {
            // If there are any unacceptable alternatives, then the remaining
            // one should be reused.
            //
            assert (!unacceptable || r.reused);

            const auto& eda (edas[first_alt->first]);
            const dependency_alternative& da (eda.first);
            size_t dai (eda.second);

            if (!collect (da, dai, move (*r.builds), prereqs))
            {
              postpone (nullptr); // Already inserted into postponed_cfgs.
              break;
            }

            select (da, dai);
          }
        }

        // If an alternative is selected, then we are done.
        //
        if (selected)
          break;

        // Fail or postpone the collection if no alternative is selected,
        // unless we are re-evaluating a dependent or are in the "recreate
        // dependency decisions" mode. In the latter case fail for
        // re-evaluation and fall back to the "make dependency decisions" mode
        // and retry otherwise.
        //
        if (prereqs != nullptr)
        {
          if (reeval)
            fail_reeval ();

          prereqs = nullptr;
          continue;
        }

        // We shouldn't end up with the "no alternative to select" case if any
        // alternatives are unacceptable.
        //
        assert (!unacceptable);

        // Issue diagnostics and fail if there are no satisfactory
        // alternatives.
        //
        if (alts_num == 0)
        {
          diag_record dr;
          for (const auto& da: edas)
            precollect (da.first, das.buildtime, nullptr /* prereqs */, &dr);

          assert (!dr.empty ());

          dr.flush ();
          throw failed ();
        }

        // Issue diagnostics and fail if there are multiple non-reused
        // alternatives or there is a single non-reused alternative in the
        // reused-only mode, unless the failure needs to be postponed.
        //
        assert (alts_num > (!reused_only ? 1 : 0));

        if (postponed_alts != nullptr)
        {
          if (verb >= 5)
          {
            diag_record dr (trace);
            dr << "alt-postpone dependent "
               << pkg.available_name_version_db ()
               << " due to ambiguous alternatives";

            for (const auto& da: edas)
              dr << info << "alternative: " << da.first;
          }

          postpone (postponed_alts);
          break;
        }

        diag_record dr (fail);

        dr << "unable to select dependency alternative for package "
           << pkg.available_name_version_db () <<
          info << "explicitly specify dependency packages to manually "
               << "select the alternative";

        for (const auto& da: edas)
        {
          precollect_result r (
            precollect (da.first, das.buildtime, nullptr /* prereqs */));

          if (r.builds)
          {
            assert (!r.reused); // We shouldn't be failing otherwise.

            dr << info << "alternative:";

            // Only print the non-reused dependencies, which needs to be
            // explicitly specified by the user.
            //
            for (const prebuild& b: *r.builds)
            {
              if (!b.reused)
                dr << ' ' << b.dependency.name;
            }
          }
        }

        // If there is only a single alternative (while we are in the
        // reused-only mode), then also print the reused unsatisfactory
        // alternatives and the reasons why they are not satisfactory.
        //
        if (alts_num == 1)
        {
          assert (reused_only);

          for (const auto& da: edas)
          {
            precollect_result r (
              precollect (da.first, das.buildtime, nullptr /* prereqs */));

            if (r.reused && r.unsatisfactory)
            {
              // Print the alternative.
              //
              dr << info << "unsatisfactory alternative:";

              for (const prebuild& b: *r.unsatisfactory)
                dr << ' ' << b.dependency.name;

              // Print the reason.
              //
              precollect (da.first,
                          das.buildtime,
                          nullptr /* prereqs */,
                          &dr);
            }
          }
        }
      }

      if (postponed)
        break;
    }

    if (reeval)
    {
      if (!reevaluated)
        fail_reeval ();

      assert (postponed);
    }

    dep_chain.pop_back ();

    l5 ([&]{trace << (!postponed ? "end "          :
                      reeval     ? "re-evaluated " :
                                   "postpone ")
                  << pkg.available_name_version_db ();});
  }

  void build_packages::
  collect_build_prerequisites (const pkg_build_options& o,
                               database& db,
                               const package_name& name,
                               const function<find_database_function>& fdb,
                               const repointed_dependents& rpt_depts,
                               const function<add_priv_cfg_function>& apc,
                               bool initial_collection,
                               replaced_versions& replaced_vers,
                               postponed_packages& postponed_repo,
                               postponed_packages& postponed_alts,
                               size_t max_alt_index,
                               postponed_dependencies& postponed_deps,
                               postponed_configurations& postponed_cfgs,
                               postponed_positions& postponed_poss,
                               unacceptable_alternatives& unacceptable_alts)
  {
    auto mi (map_.find (db, name));
    assert (mi != map_.end ());

    build_package_refs dep_chain;

    collect_build_prerequisites (o,
                                 mi->second.package,
                                 fdb,
                                 rpt_depts,
                                 apc,
                                 initial_collection,
                                 replaced_vers,
                                 dep_chain,
                                 &postponed_repo,
                                 &postponed_alts,
                                 max_alt_index,
                                 postponed_deps,
                                 postponed_cfgs,
                                 postponed_poss,
                                 unacceptable_alts);
  }

  void build_packages::
  collect_repointed_dependents (const pkg_build_options& o,
                                const repointed_dependents& rpt_depts,
                                replaced_versions& replaced_vers,
                                postponed_packages& postponed_repo,
                                postponed_packages& postponed_alts,
                                postponed_dependencies& postponed_deps,
                                postponed_configurations& postponed_cfgs,
                                postponed_positions& postponed_poss,
                                unacceptable_alternatives& unacceptable_alts,
                                const function<find_database_function>& fdb,
                                const function<add_priv_cfg_function>& apc)
  {
    for (const auto& rd: rpt_depts)
    {
      database&           db (rd.first.db);
      const package_name& nm (rd.first.name);

      auto i (map_.find (db, nm));
      if (i != map_.end ())
      {
        build_package& b (i->second.package);

        if (!b.action || *b.action != build_package::adjust)
        {
          if (!b.action ||
              (*b.action != build_package::drop && !b.reconfigure ()))
            b.flags |= build_package::adjust_reconfigure;

          continue;
        }
      }

      shared_ptr<selected_package> sp (db.load<selected_package> (nm));

      // The repointed dependent can be an orphan, so just create the
      // available package from the selected package.
      //
      auto rp (make_available_fragment (o, db, sp));

      // Add the prerequisite replacements as the required-by packages.
      //
      set<package_key> required_by;
      for (const auto& prq: rd.second)
      {
        if (prq.second) // Prerequisite replacement?
        {
          const package_key& pk (prq.first);
          required_by.emplace (pk.db, pk.name);
        }
      }

      build_package p {
        build_package::build,
          db,
          sp,
          move (rp.first),
          move (rp.second),
          nullopt,                    // Dependencies.
          nullopt,                    // Dependencies alternatives.
          nullopt,                    // Package skeleton.
          nullopt,                    // Postponed dependency alternatives.
          false,                      // Recursive collection.
          nullopt,                    // Hold package.
          nullopt,                    // Hold version.
          {},                         // Constraints.
          sp->system (),
          false,                      // Keep output directory.
          false,                      // Disfigure (from-scratch reconf).
          false,                      // Configure-only.
          nullopt,                    // Checkout root.
          false,                      // Checkout purge.
          strings (),                 // Configuration variables.
          move (required_by),         // Required by (dependencies).
          false,                      // Required by dependents.
          build_package::adjust_reconfigure | build_package::build_repoint};

      build_package_refs dep_chain;

      // Note: recursive.
      //
      collect_build (o,
                     move (p),
                     fdb,
                     rpt_depts,
                     apc,
                     true /* initial_collection */,
                     replaced_vers,
                     postponed_cfgs,
                     &dep_chain,
                     &postponed_repo,
                     &postponed_alts,
                     &postponed_deps,
                     &postponed_poss,
                     &unacceptable_alts);
    }
  }

  void build_packages::
  collect_drop (const pkg_build_options& options,
                database& db,
                shared_ptr<selected_package> sp,
                replaced_versions& replaced_vers)
  {
    tracer trace ("collect_drop");

    package_key pk (db, sp->name);

    // If there is an entry for building specific version of the package (the
    // available member is not NULL), then it wasn't created to prevent out
    // drop (see replaced_versions for details). This rather mean that the
    // replacement version is not being built anymore due to the plan
    // refinement. Thus, just erase the entry in this case and continue.
    //
    auto vi (replaced_vers.find (pk));
    if (vi != replaced_vers.end () && !vi->second.replaced)
    {
      replaced_version& v (vi->second);
      const shared_ptr<available_package>& ap (v.available);

      if (ap != nullptr)
      {
        if (verb >= 5)
        {
          bool s (v.system);
          const version& av (s ? *ap->system_version (db) : ap->version);

          l5 ([&]{trace << "erase version replacement for "
                        << package_string (ap->id.name, av, s) << db;});
        }

        replaced_vers.erase (vi);
        vi = replaced_vers.end (); // Keep it valid for the below check.
      }
      else
        v.replaced = true;
    }

    build_package p {
      build_package::drop,
        db,
        move (sp),
        nullptr,
        nullptr,
        nullopt,    // Dependencies.
        nullopt,    // Dependencies alternatives.
        nullopt,    // Package skeleton.
        nullopt,    // Postponed dependency alternatives.
        false,      // Recursive collection.
        nullopt,    // Hold package.
        nullopt,    // Hold version.
        {},         // Constraints.
        false,      // System package.
        false,      // Keep output directory.
        false,      // Disfigure (from-scratch reconf).
        false,      // Configure-only.
        nullopt,    // Checkout root.
        false,      // Checkout purge.
        strings (), // Configuration variables.
        {},         // Required by.
        false,      // Required by dependents.
        0};         // State flags.

    auto i (map_.find (pk));

    if (i != map_.end ())
    {
      build_package& bp (i->second.package);

      if (bp.available != nullptr)
      {
        // Similar to the version replacement in collect_build(), see if
        // in-place drop is possible (no dependencies, etc) and set scratch to
        // false if that's the case.
        //
        bool scratch (true);

        // While checking if the package has any dependencies skip the
        // toolchain build-time dependencies since they should be quite
        // common.
        //
        if (!has_dependencies (options, bp.available->dependencies))
          scratch = false;

        l5 ([&]{trace << bp.available_name_version_db ()
                      << " package version needs to be replaced "
                      << (!scratch ? "in-place " : "") << "with drop";});

        if (scratch)
        {
          if (vi != replaced_vers.end ())
            vi->second = replaced_version ();
          else
            replaced_vers.emplace (move (pk), replaced_version ());

          throw replace_version ();
        }
      }

      // Overwrite the existing (possibly pre-entered, adjustment, or repoint)
      // entry.
      //
      l4 ([&]{trace << "overwrite " << pk;});

      bp = move (p);
    }
    else
    {
      l4 ([&]{trace << "add " << pk;});

      map_.emplace (move (pk), data_type {end (), move (p)});
    }
  }

  void build_packages::
  collect_unhold (database& db, const shared_ptr<selected_package>& sp)
  {
    auto i (map_.find (db, sp->name));

    // Currently, it must always be pre-entered.
    //
    assert (i != map_.end ());

    build_package& bp (i->second.package);

    if (!bp.action) // Pre-entered.
    {
      build_package p {
        build_package::adjust,
          db,
          sp,
          nullptr,
          nullptr,
          nullopt,       // Dependencies.
          nullopt,       // Dependencies alternatives.
          nullopt,       // Package skeleton.
          nullopt,       // Postponed dependency alternatives.
          false,         // Recursive collection.
          nullopt,       // Hold package.
          nullopt,       // Hold version.
          {},            // Constraints.
          sp->system (),
          false,         // Keep output directory.
          false,         // Disfigure (from-scratch reconf).
          false,         // Configure-only.
          nullopt,       // Checkout root.
          false,         // Checkout purge.
          strings (),    // Configuration variables.
          {},            // Required by.
          false,         // Required by dependents.
          build_package::adjust_unhold};

      p.merge (move (bp));
      bp = move (p);
    }
    else
      bp.flags |= build_package::adjust_unhold;
  }

  void build_packages::
  collect_build_postponed (const pkg_build_options& o,
                           replaced_versions& replaced_vers,
                           postponed_packages& postponed_repo,
                           postponed_packages& postponed_alts,
                           postponed_dependencies& postponed_deps,
                           postponed_configurations& postponed_cfgs,
                           strings& postponed_cfgs_history,
                           postponed_positions& postponed_poss,
                           unacceptable_alternatives& unacceptable_alts,
                           const function<find_database_function>& fdb,
                           const repointed_dependents& rpt_depts,
                           const function<add_priv_cfg_function>& apc,
                           postponed_configuration* pcfg)
  {
    // Snapshot of the package builds collection state.
    //
    // Note: should not include postponed_cfgs_history.
    //
    class snapshot
    {
    public:
      snapshot (const build_packages& pkgs,
                const postponed_packages& postponed_repo,
                const postponed_packages& postponed_alts,
                const postponed_dependencies& postponed_deps,
                const postponed_configurations& postponed_cfgs)
          : pkgs_ (pkgs),
            postponed_deps_ (postponed_deps),
            postponed_cfgs_ (postponed_cfgs)
      {
        auto save = [] (vector<package_key>& d, const postponed_packages& s)
        {
          d.reserve (s.size ());

          for (const build_package* p: s)
            d.emplace_back (p->db, p->name ());
        };

        save (postponed_repo_, postponed_repo);
        save (postponed_alts_, postponed_alts);
      }

      void
      restore (build_packages& pkgs,
               postponed_packages& postponed_repo,
               postponed_packages& postponed_alts,
               postponed_dependencies& postponed_deps,
               postponed_configurations& postponed_cfgs)
      {
        pkgs           = move (pkgs_);
        postponed_cfgs = move (postponed_cfgs_);
        postponed_deps = move (postponed_deps_);

        auto restore = [&pkgs] (postponed_packages& d,
                                const vector<package_key>& s)
        {
          d.clear ();

          for (const package_key& p: s)
          {
            build_package* b (pkgs.entered_build (p));
            assert (b != nullptr);
            d.insert (b);
          }
        };

        restore (postponed_repo, postponed_repo_);
        restore (postponed_alts, postponed_alts_);
      }

    private:
      // Note: try to use vectors instead of sets for storage to save
      //       memory. We could probably optimize this some more if necessary
      //       (there are still sets/maps inside).
      //
      build_packages           pkgs_;
      vector<package_key>      postponed_repo_;
      vector<package_key>      postponed_alts_;
      postponed_dependencies   postponed_deps_;
      postponed_configurations postponed_cfgs_;
    };

    // This exception is thrown if negotiation of the current cluster needs to
    // be skipped until later. This is normally required if this cluster
    // contains some existing dependent which needs to be re-evaluated to a
    // dependency position greater than some other not yet negotiated cluster
    // will re-evaluate this dependent to. Sometimes this another cluster yet
    // needs to be created in which case the exception carries the information
    // required for that (see the postponed_position's replace flag for
    // details).
    //
    struct skip_configuration
    {
      optional<existing_dependent> dependent;
      pair<size_t, size_t> new_position;

      skip_configuration () = default;

      skip_configuration (existing_dependent&& d, pair<size_t, size_t> n)
          : dependent (move (d)), new_position (n) {}
    };

    size_t depth (pcfg != nullptr ? pcfg->depth : 0);

    string t ("collect_build_postponed (" + to_string (depth) + ')');
    tracer trace (t.c_str ());

    string trace_suffix;
    if (verb >= 5 && pcfg != nullptr)
    {
      trace_suffix += ' ';
      trace_suffix += pcfg->string ();
    }

    l5 ([&]{trace << "begin" << trace_suffix;});

    if (pcfg != nullptr)
    {
      // This is what we refer to as the "initial negotiation" where we
      // negotiate the configuration of dependents that could be postponed.
      // Those that could not we "up-negotiate" in the collect() lambda of
      // collect_build_prerequisites().
      //
      using packages = postponed_configuration::packages;

      assert (!pcfg->negotiated);

      // Re-evaluate existing dependents with configuration clause for
      // dependencies in this configuration cluster up to these
      // dependencies. Omit dependents which are already being built or
      // dropped. Note that these dependents, potentially with additional
      // dependencies, will be added to this cluster with the `existing` flag
      // as a part of the dependents' re-evaluation (see the collect lambda in
      // collect_build_prerequisites() for details).
      //
      // After being re-evaluated the existing dependents are recursively
      // collected in the same way as the new dependents.
      //
      {
        // Map existing dependents to the dependencies they apply a
        // configuration to. Also, collect the information which is required
        // for a dependent re-evaluation and its subsequent recursive
        // collection (selected package, etc).
        //
        // As mentioned earlier, we may end up adding additional dependencies
        // to pcfg->dependencies which in turn may have additional existing
        // dependents which we need to process. Feels like doing this
        // iteratively is the best option.
        //
        // Note that we need to make sure we don't re-process the same
        // existing dependents.
        //
        struct existing_dependent_ex: existing_dependent
        {
          packages dependencies;
          bool     reevaluated = false;

          existing_dependent_ex (existing_dependent&& ed)
              : existing_dependent (move (ed)) {}
        };
        map<package_key, existing_dependent_ex> dependents;

        const packages& deps (pcfg->dependencies);

        // Note that the below collect_build_prerequisites() call can only add
        // new dependencies to the end of the cluster's dependencies
        // list. Thus on each iteration we will only add existing dependents
        // of unprocessed/new dependencies. We will also skip the already
        // re-evaluated existing dependents.
        //
        for (size_t i (0); i != deps.size (); )
        {
          size_t n (dependents.size ());

          for (; i != deps.size (); ++i)
          {
            // Note: this reference is only used while deps is unchanged.
            //
            const package_key& p (deps[i]);

            // If the dependent is being built, then check if it was
            // re-evaluated to the position greater than the dependency
            // position. Return true if that's the case, so this package is
            // added to the resulting list and we can handle this situation.
            //
            // Note that we rely on "small function object" optimization
            // here.
            //
            const function<verify_dependent_build_function> verify (
              [&postponed_cfgs, pcfg]
              (const package_key& pk, pair<size_t, size_t> pos)
              {
                for (const postponed_configuration& cfg: postponed_cfgs)
                {
                  if (&cfg == pcfg || cfg.negotiated)
                  {
                    if (const pair<size_t, size_t>* p =
                        cfg.existing_dependent_position (pk))
                    {
                      if (p->first > pos.first)
                        return true;
                    }
                  }
                }

                return false;
              });

            for (existing_dependent& ed:
                   query_existing_dependents (trace,
                                              p.db,
                                              p.name,
                                              replaced_vers,
                                              rpt_depts,
                                              verify))
            {
              package_key pk (ed.db, ed.selected->name);

              // If this dependent is present in postponed_deps, then it means
              // someone depends on it with configuration and it's no longer
              // considered an existing dependent (it will be reconfigured).
              // However, this fact may not be reflected yet. And it can
              // actually turn out bogus.
              //
              auto pi (postponed_deps.find (pk));
              if (pi != postponed_deps.end ())
              {
                l5 ([&]{trace << "skip dep-postponed existing dependent "
                              << pk << " of dependency " << p;});

                // Note that here we would re-evaluate the existing dependent
                // without specifying any configuration for it.
                //
                pi->second.wout_config = true;

                continue;
              }

              auto i (dependents.find (pk));
              size_t di (ed.dependency_position.first);

              // Skip re-evaluated dependent if the dependency index is
              // greater than the one we have already re-evaluated to. If it
              // is earlier, then add the entry to postponed_poss and throw
              // postpone_position to recollect from scratch. Note that this
              // entry in postponed_poss is with replacement.
              //
              if (i != dependents.end () && i->second.reevaluated)
              {
                size_t ci (i->second.dependency_position.first);

                if (di > ci)
                  continue;

                // The newly-introduced dependency must belong to the depends
                // value other then the one we have re-evaluated to.
                //
                assert (di < ci);

                postponed_position pp (ed.dependency_position,
                                       true /* replace */);

                auto p (postponed_poss.emplace (pk, pp));

                if (!p.second)
                {
                  assert (p.first->second > pp);
                  p.first->second = pp;
                }

                l5 ([&]{trace << "cannot re-evaluate dependent "
                              << pk << " to dependency index " << di
                              << " since it is already re-evaluated to "
                              << "greater index " << ci << " in " << *pcfg
                              << ", throwing postpone_position";});

                throw postpone_position ();
              }

              // If the existing dependent is not in the map yet, then add
              // it. Otherwise, if the dependency position is greater than
              // that one in the existing map entry then skip it (this
              // position will be up-negotiated, if it's still present).
              // Otherwise, if the position is less then overwrite the
              // existing entry. Otherwise (the position is equal), just add
              // the dependency to the existing entry.
              //
              // Note that we want to re-evaluate the dependent up to the
              // earliest dependency position and continue with the regular
              // prerequisites collection (as we do for new dependents)
              // afterwards.
              //
              if (i == dependents.end ())
              {
                i = dependents.emplace (
                  move (pk), existing_dependent_ex (move (ed))).first;
              }
              else
              {
                size_t ci (i->second.dependency_position.first);

                if (ci < di)
                  continue;
                else if (ci > di)
                  i->second = existing_dependent_ex (move (ed));
                //else if (ci == di)
                //  ;
              }

              i->second.dependencies.push_back (p);
            }
          }

          // Re-evaluate the newly added existing dependents, if any.
          //
          if (dependents.size () != n)
          {
            l5 ([&]{trace << "re-evaluate existing dependents for " << *pcfg;});

            for (auto& d: dependents)
            {
              existing_dependent_ex& ed (d.second);

              // Skip re-evaluated.
              //
              if (ed.reevaluated)
                continue;

              size_t di (ed.dependency_position.first);
              const package_key& pk (d.first);

              // Check if there is an earlier dependency position for this
              // dependent that will be participating in a configuration
              // negotiation and skip this cluster if that's the case. There
              // are two places to check: postponed_poss and other clusters.
              //
              auto pi (postponed_poss.find (pk));
              if (pi != postponed_poss.end () && pi->second.first < di)
              {
                l5 ([&]{trace << "pos-postpone existing dependent "
                              << pk << " re-evaluation to dependency "
                              << "index " << di << " due to recorded index "
                              << pi->second.first << ", skipping " << *pcfg;});

                pi->second.skipped = true;

                // If requested, override the first encountered non-replace
                // position to replace (see below for details).
                //
                if (!pi->second.replace && postponed_poss.replace)
                {
                  pi->second.replace = true;
                  postponed_poss.replace = false;
                }

                if (pi->second.replace)
                  throw skip_configuration (move (ed), pi->second);
                else
                  throw skip_configuration ();
              }

              // The other clusters check is a bit more complicated: if the
              // other cluster (with the earlier position) is not yet
              // negotiated, then we skip. Otherwise, we have to add an entry
              // to postponed_poss and backtrack.
              //
              bool skip (false);
              for (const postponed_configuration& cfg: postponed_cfgs)
              {
                // Skip the current cluster.
                //
                if (&cfg == pcfg)
                  continue;

                if (const pair<size_t, size_t>* p =
                    cfg.existing_dependent_position (pk))
                {
                  size_t ei (p->first); // Other position.

                  if (!cfg.negotiated)
                  {
                    if (ei < di)
                    {
                      l5 ([&]{trace << "cannot re-evaluate dependent "
                                    << pk << " to dependency index " << di
                                    << " due to earlier dependency index "
                                    << ei << " in " << cfg << ", skipping "
                                    << *pcfg;});

                      skip = true;
                    }
                  }
                  else
                  {
                    // If this were not the case, then this dependent wouldn't
                    // have been considered as an existing by
                    // query_existing_dependents() since as it is (being)
                    // negotiated then it is already re-evaluated and so is
                    // being built (see the verify lambda above).
                    //
                    assert (ei > di);

                    // Feels like there cannot be an earlier position.
                    //
                    postponed_position pp (ed.dependency_position,
                                           false /* replace */);

                    auto p (postponed_poss.emplace (pk, pp));
                    if (!p.second)
                    {
                      assert (p.first->second > pp);
                      p.first->second = pp;
                    }

                    l5 ([&]{trace << "cannot re-evaluate dependent "
                                  << pk << " to dependency index " << di
                                  << " due to greater dependency "
                                  << "index " << ei << " in " << cfg
                                  << ", throwing postpone_position";});

                    throw postpone_position ();
                  }
                }
              }

              if (skip)
                throw skip_configuration ();

              // Finally, re-evaluate the dependent.
              //
              packages& ds (ed.dependencies);

              pair<shared_ptr<available_package>,
                   lazy_shared_ptr<repository_fragment>> rp (
                     find_available_fragment (o, pk.db, ed.selected));

              build_package p {
                build_package::build,
                  pk.db,
                  move (ed.selected),
                  move (rp.first),
                  move (rp.second),
                  nullopt,                    // Dependencies.
                  nullopt,                    // Dependencies alternatives.
                  nullopt,                    // Package skeleton.
                  nullopt,                    // Postponed dependency alternatives.
                  false,                      // Recursive collection.
                  nullopt,                    // Hold package.
                  nullopt,                    // Hold version.
                  {},                         // Constraints.
                  false,                      // System.
                  false,                      // Keep output directory.
                  false,                      // Disfigure (from-scratch reconf).
                  false,                      // Configure-only.
                  nullopt,                    // Checkout root.
                  false,                      // Checkout purge.
                  strings (),                 // Configuration variables.
                  set<package_key> (          // Required by (dependency).
                    ds.begin (), ds.end ()),
                  false,                      // Required by dependents.
                  build_package::adjust_reconfigure |
                  build_package::build_reevaluate};

              // Note: not recursive.
              //
              collect_build (o,
                             move (p),
                             fdb,
                             rpt_depts,
                             apc,
                             false /* initial_collection */,
                             replaced_vers,
                             postponed_cfgs);

              build_package* b (entered_build (pk));
              assert (b != nullptr);

              // Re-evaluate up to the earliest position.
              //
              assert (ed.dependency_position.first != 0);

              build_package_refs dep_chain;
              collect_build_prerequisites (o,
                                           *b,
                                           fdb,
                                           rpt_depts,
                                           apc,
                                           false /* initial_collection */,
                                           replaced_vers,
                                           dep_chain,
                                           &postponed_repo,
                                           &postponed_alts,
                                           numeric_limits<size_t>::max (),
                                           postponed_deps,
                                           postponed_cfgs,
                                           postponed_poss,
                                           unacceptable_alts,
                                           ed.dependency_position);

              ed.reevaluated = true;

              if (pi != postponed_poss.end ())
              {
                // Otherwise we should have thrown skip_configuration above.
                //
                assert (di <= pi->second.first);

                pi->second.reevaluated = true;
              }
            }
          }
        }
      }

      l5 ([&]{trace << "cfg-negotiate begin " << *pcfg;});

      // Negotiate the configuration.
      //
      // The overall plan is as follows: continue refining the configuration
      // until there are no more changes by giving each dependent a chance to
      // make further adjustments.
      //
      for (auto b (pcfg->dependents.begin ()),
                i (b),
                e (pcfg->dependents.end ()); i != e; )
      {
        // Resolve package skeletons for the dependent and its dependencies.
        //
        // For the dependent, the skeleton should be already there (since we
        // should have started recursively collecting it). For a dependency,
        // it should not already be there (since we haven't yet started
        // recursively collecting it). But we could be re-resolving the same
        // dependency multiple times.
        //
        package_skeleton* dept;
        {
          build_package* b (entered_build (i->first));
          assert (b != nullptr && b->skeleton);
          dept = &*b->skeleton;
        }

        pair<size_t, size_t> pos;
        small_vector<reference_wrapper<package_skeleton>, 1> depcs;
        bool has_alt;
        {
          // A non-negotiated cluster must only have one depends position for
          // each dependent.
          //
          assert (i->second.dependencies.size () == 1);

          const postponed_configuration::dependency& ds (
            i->second.dependencies.front ());

          pos = ds.position;

          // Note that an existing dependent which initially doesn't have the
          // has_alternative flag present should obtain it as a part of
          // re-evaluation at this time.
          //
          assert (ds.has_alternative);

          has_alt = *ds.has_alternative;

          depcs.reserve (ds.size ());
          for (const package_key& pk: ds)
          {
            build_package* b (entered_build (pk));
            assert (b != nullptr);

            depcs.push_back (b->skeleton
                             ? *b->skeleton
                             : b->init_skeleton (o /* options */));
          }
        }

        optional<bool> changed (
          negotiate_configuration (
            pcfg->dependency_configurations, *dept, pos, depcs, has_alt));

        // If the dependency alternative configuration cannot be negotiated
        // for this dependent, then add an entry to unacceptable_alts and
        // throw unaccept_alternative to recollect from scratch.
        //
        if (!changed)
        {
          assert (dept->available != nullptr); // Can't be system.

          const package_key& p (dept->package);
          const version& v (dept->available->version);

          unacceptable_alts.emplace (p, v, pos);

          l5 ([&]{trace << "unable to cfg-negotiate dependency alternative "
                        << pos.first << ',' << pos.second << " for "
                        << "dependent " << package_string (p.name, v)
                        << p.db << ", throwing unaccept_alternative";});

          throw unaccept_alternative ();
        }
        else if (*changed)
        {
          if (i != b)
          {
            i = b; // Restart from the beginning.
            continue;
          }
        }

        ++i;
      }

      // Being negotiated (so can only be up-negotiated).
      //
      pcfg->negotiated = false;

      // Note that we can be adding new packages to the being negotiated
      // cluster by calling collect_build_prerequisites() for its dependencies
      // and dependents. Thus, we need to stash the current list of
      // dependencies and dependents and iterate over them.
      //
      // Note that whomever is adding new packages is expected to process them
      // (they may also process existing packages, which we are prepared to
      // ignore).
      //
      packages dependencies (pcfg->dependencies);

      packages dependents;
      dependents.reserve (pcfg->dependents.size ());

      for (const auto& p: pcfg->dependents)
        dependents.push_back (p.first);

      // Process dependencies recursively with this config.
      //
      // Note that there could be inter-dependecies between these packages,
      // which means the configuration can only be up-negotiated.
      //
      l5 ([&]{trace << "recursively collect cfg-negotiated dependencies";});

      for (const package_key& p: dependencies)
      {
        build_package* b (entered_build (p));
        assert (b != nullptr);

        // Reconfigure the configured dependencies.
        //
        // Note that potentially this can be an overkill if the dependency
        // configuration doesn't really change. Later we can implement some
        // precise detection for that using configuration checksum or similar.
        //
        // Also note that for configured dependents which belong to the
        // configuration cluster this flag is already set (see above).
        //
        if (b->selected != nullptr &&
            b->selected->state == package_state::configured)
          b->flags |= build_package::adjust_reconfigure;

        // Skip the dependencies which are already collected recursively.
        //
        if (!b->recursive_collection)
        {
          // Verify and set the dependent configuration for this dependency.
          //
          // Note: see similar code for the up-negotiation case.
          //
          {
            assert (b->skeleton); // Should have been init'ed above.

            const package_configuration& pc (
              pcfg->dependency_configurations[p]);

            // Skip the verification if this is a system package without
            // skeleton info.
            //
            pair<bool, string> pr (b->skeleton->available != nullptr
                                   ? b->skeleton->verify_sensible (pc)
                                   : make_pair (true, string ()));

            if (!pr.first)
            {
              // Note that the diagnostics from the dependency will most
              // likely be in the "error ..." form (potentially with
              // additional info lines) and by printing it with a two-space
              // indentation we make it "fit" into our diag record.
              //
              diag_record dr (fail);
              dr << "unable to negotiate sensible configuration for "
                 << "dependency " << p << '\n'
                 << "  " << pr.second;

              dr << info << "negotiated configuration:\n";
              pc.print (dr, "    "); // Note 4 spaces since in nested info.
            }

            b->skeleton->dependent_config (pc);
          }

          build_package_refs dep_chain;
          collect_build_prerequisites (o,
                                       *b,
                                       fdb,
                                       rpt_depts,
                                       apc,
                                       false /* initial_collection */,
                                       replaced_vers,
                                       dep_chain,
                                       &postponed_repo,
                                       &postponed_alts,
                                       0 /* max_alt_index */,
                                       postponed_deps,
                                       postponed_cfgs,
                                       postponed_poss,
                                       unacceptable_alts);
        }
        else
          l5 ([&]{trace << "dependency " << b->available_name_version_db ()
                        << " is already (being) recursively collected, "
                        << "skipping";});
      }

      // Continue processing dependents with this config.
      //
      l5 ([&]{trace << "recursively collect cfg-negotiated dependents";});

      for (const auto& p: dependents)
      {
        // Select the dependency alternative for which configuration has been
        // negotiated and collect this dependent starting from the next
        // depends value.
        //
        build_package* b (entered_build (p));

        // We should have been started recursively collecting the dependent
        // and it should have been postponed.
        //
        assert (b != nullptr            &&
                b->available != nullptr &&
                b->dependencies         &&
                b->skeleton             &&
                b->postponed_dependency_alternatives);

        // Select the dependency alternative (evaluate reflect if present,
        // etc) and position to the next depends value (see
        // collect_build_prerequisites() for details).
        //
        {
          const bpkg::dependencies& deps  (b->available->dependencies);
          bpkg::dependencies&       sdeps (*b->dependencies);
          vector<size_t>&           salts (*b->alternatives);

          size_t di (sdeps.size ());

          // Skip the dependent if it has been already collected as some
          // package's dependency or some such.
          //
          if (di == deps.size ())
          {
            l5 ([&]{trace << "dependent " << b->available_name_version_db ()
                          << " is already recursively collected, skipping";});

            continue;
          }

          l5 ([&]{trace << "select cfg-negotiated dependency alternative "
                        << "for dependent "
                        << b->available_name_version_db ();});

          // Find the postponed dependency alternative.
          //
          auto i (pcfg->dependents.find (p));

          assert (i != pcfg->dependents.end () &&
                  i->second.dependencies.size () == 1);

          pair<size_t, size_t> dp (i->second.dependencies[0].position);
          assert (dp.first == sdeps.size () + 1);

          build_package::dependency_alternatives_refs pdas (
            move (*b->postponed_dependency_alternatives));

          b->postponed_dependency_alternatives = nullopt;

          auto j (find_if (pdas.begin (), pdas.end (),
                           [&dp] (const auto& da)
                           {
                             return da.second + 1 == dp.second;
                           }));

          assert (j != pdas.end ());

          const dependency_alternative& da (j->first);
          size_t dai (j->second);

          // Select the dependency alternative and position to the next
          // depends value.
          //
          const dependency_alternatives_ex& das (deps[di]);
          dependency_alternatives_ex sdas (das.buildtime, das.comment);

          sdas.emplace_back (nullopt /* enable */,
                             nullopt /* reflect */,
                             da.prefer,
                             da.accept,
                             da.require,
                             da /* dependencies */);

          sdeps.push_back (move (sdas));
          salts.push_back (dai);

          // Evaluate reflect, if present.
          //
          if (da.reflect)
            b->skeleton->evaluate_reflect (*da.reflect, make_pair (di, dai));
        }

        // Continue recursively collecting the dependent.
        //
        build_package_refs dep_chain;

        collect_build_prerequisites (o,
                                     *b,
                                     fdb,
                                     rpt_depts,
                                     apc,
                                     false /* initial_collection */,
                                     replaced_vers,
                                     dep_chain,
                                     &postponed_repo,
                                     &postponed_alts,
                                     0 /* max_alt_index */,
                                     postponed_deps,
                                     postponed_cfgs,
                                     postponed_poss,
                                     unacceptable_alts);
      }

      // Negotiated (so can only be rolled back).
      //
      pcfg->negotiated = true;

      l5 ([&]{trace << "cfg-negotiate end " << *pcfg;});

      // Fall through (to start another iteration of the below loop).
    }

    // Try collecting postponed packages for as long as we are making
    // progress.
    //
    vector<build_package*> spas; // Reuse.

    for (bool prog (!postponed_repo.empty ()      ||
                    !postponed_cfgs.negotiated () ||
                    !postponed_alts.empty ()      ||
                    postponed_deps.has_bogus ());
         prog; )
    {
      postponed_packages prs;
      postponed_packages pas;

      // Try to collect the repository-related postponments first.
      //
      for (build_package* p: postponed_repo)
      {
        l5 ([&]{trace << "collect rep-postponed "
                      << p->available_name_version_db ();});

        build_package_refs dep_chain;

        collect_build_prerequisites (o,
                                     *p,
                                     fdb,
                                     rpt_depts,
                                     apc,
                                     false /* initial_collection */,
                                     replaced_vers,
                                     dep_chain,
                                     &prs,
                                     &pas,
                                     0 /* max_alt_index */,
                                     postponed_deps,
                                     postponed_cfgs,
                                     postponed_poss,
                                     unacceptable_alts);
      }

      // Save the potential new dependency alternative-related postponements.
      //
      postponed_alts.insert (pas.begin (), pas.end ());

      prog = (prs != postponed_repo);

      if (prog)
      {
        postponed_repo.swap (prs);
        continue;
      }

      // Now, as there is no more progress made in collecting repository-
      // related postponements, collect the dependency configuration-related
      // postponements.
      //
      // Note that we do it before alternatives since configurations we do
      // perfectly (via backtracking) while alternatives -- heuristically.
      //
      // Note that since the potential snapshot restore replaces all the list
      // entries we cannot iterate using the iterator here. Also note that the
      // list size may change during iterating.
      //
      for (size_t ci (0); ci != postponed_cfgs.size (); ++ci)
      {
        postponed_configuration* pc (&postponed_cfgs[ci]);

        // Find the next configuration to try to negotiate, skipping the
        // already negotiated ones.
        //
        if (pc->negotiated)
          continue;

        size_t pcd (depth + 1);
        pc->depth = pcd;

        // Either return or retry the same cluster or skip this cluster and
        // proceed to the next one.
        //
        for (;;)
        {
          // First assume we can negotiate this configuration rolling back if
          // this doesn't pan out.
          //
          snapshot s (*this,
                      postponed_repo,
                      postponed_alts,
                      postponed_deps,
                      postponed_cfgs);

          try
          {
            collect_build_postponed (o,
                                     replaced_vers,
                                     postponed_repo,
                                     postponed_alts,
                                     postponed_deps,
                                     postponed_cfgs,
                                     postponed_cfgs_history,
                                     postponed_poss,
                                     unacceptable_alts,
                                     fdb,
                                     rpt_depts,
                                     apc,
                                     pc);

            // If collect() returns (instead of throwing), this means it
            // processed everything that was postponed.
            //
            assert (postponed_repo.empty ()      &&
                    postponed_cfgs.negotiated () &&
                    postponed_alts.empty ()      &&
                    !postponed_deps.has_bogus ());

            l5 ([&]{trace << "end" << trace_suffix;});

            return;
          }
          catch (skip_configuration& e)
          {
            // Restore the state from snapshot.
            //
            // Note: postponed_cfgs is re-assigned.
            //
            s.restore (*this,
                       postponed_repo,
                       postponed_alts,
                       postponed_deps,
                       postponed_cfgs);

            pc = &postponed_cfgs[ci];

            // Note that in this case we keep the accumulated configuration,
            // if any.

            pc->depth = 0;

            // If requested, "replace" the "later" dependent-dependency
            // cluster with an earlier.
            //
            if (e.dependent)
            {
              existing_dependent& ed (*e.dependent);
              pair<size_t, size_t> pos (e.new_position);

              const build_package* bp (
                replace_existing_dependent_dependency (
                  trace,
                  o,
                  ed, // Note: modified.
                  pos,
                  fdb,
                  rpt_depts,
                  apc,
                  false /* initial_collection */,
                  replaced_vers,
                  postponed_cfgs));

              postponed_cfgs.add (package_key (ed.db, ed.selected->name),
                                  pos,
                                  package_key (bp->db, bp->selected->name));
            }

            l5 ([&]{trace << "postpone cfg-negotiation of " << *pc;});

            break;
          }
          catch (const retry_configuration& e)
          {
            // If this is not "our problem", then keep looking.
            //
            if (e.depth != pcd)
              throw;

            package_configurations cfgs (
              move (pc->dependency_configurations));

            // Restore the state from snapshot.
            //
            // Note: postponed_cfgs is re-assigned.
            //
            s.restore (*this,
                       postponed_repo,
                       postponed_alts,
                       postponed_deps,
                       postponed_cfgs);

            pc = &postponed_cfgs[ci];

            l5 ([&]{trace << "cfg-negotiation of " << *pc << " failed due "
                          << "to dependent " << e.dependent << ", refining "
                          << "configuration";});

            // Copy over the configuration for further refinement.
            //
            // Note that there is also a possibility of ending up with "bogus"
            // configuration variables that were set by a dependent during
            // up-negotiation but, due to changes to the overall
            // configuration, such a dependent were never re-visited.
            //
            // The way we are going to deal with this is by detecting such
            // bogus variables based on the confirmed flag, cleaning them out,
            // and doing another retry. Here we clear the confirmed flag and
            // the detection happens in collect_build_postponed() after we
            // have processed everything postponed (since that's the only time
            // we can be certain there could no longer be a re-visit).
            //
            for (package_configuration& cfg: cfgs)
              for (config_variable_value& v: cfg)
                if (v.dependent)
                  v.confirmed = false;

            pc->dependency_configurations = move (cfgs);
          }
          catch (merge_configuration& e)
          {
            // If this is not "our problem", then keep looking.
            //
            if (e.depth != pcd)
              throw;

            postponed_configuration shadow (move (*pc));

            // Restore the state from snapshot.
            //
            // Note: postponed_cfgs is re-assigned.
            //
            s.restore (*this,
                       postponed_repo,
                       postponed_alts,
                       postponed_deps,
                       postponed_cfgs);

            pc = &postponed_cfgs[ci];

            assert (!pc->negotiated);

            // Drop any accumulated configuration (which could be carried
            // over from retry_configuration logic).
            //
            pc->dependency_configurations.clear ();

            l5 ([&]{trace << "cfg-negotiation of " << *pc << " failed due "
                          << "to non-negotiated clusters, force-merging "
                          << "based on shadow cluster " << shadow;});

            // Pre-merge into this cluster those non-negotiated clusters which
            // were merged into the shadow cluster.
            //
            for (size_t id: shadow.merged_ids)
            {
              postponed_configuration* c (postponed_cfgs.find (id));

              if (c != nullptr)
              {
                // Otherwise we would be handling the exception in the higher
                // stack frame.
                //
                assert (!c->negotiated);

                l5 ([&]{trace << "force-merge " << *c << " into " << *pc;});

                pc->merge (move (*c));

                // Mark configuration as the one being merged from for
                // subsequent erasing from the list.
                //
                c->dependencies.clear ();
              }
            }

            // Erase clusters which we have merged from. Also re-translate the
            // current cluster address into index which may change as a result
            // of the merge.
            //
            auto i (postponed_cfgs.begin ());
            auto j (postponed_cfgs.before_begin ()); // Precedes iterator i.

            for (size_t k (0); i != postponed_cfgs.end (); )
            {
              if (!i->dependencies.empty ())
              {
                if (&*i == pc)
                  ci = k;

                ++i;
                ++j;
                ++k;
              }
              else
                i = postponed_cfgs.erase_after (j);
            }

            pc->set_shadow_cluster (move (shadow));
          }
        }
      }

      // Note that we only get here if we didn't make any progress on the
      // previous loop (the only "progress" path ends with return).

      // Now, try to collect the dependency alternative-related
      // postponements.
      //
      if (!postponed_alts.empty ())
      {
        // Sort the postponments in the unprocessed dependencies count
        // descending order.
        //
        // The idea here is to preferably handle those postponed packages
        // first, which have a higher probability to affect the dependency
        // alternative selection for other packages.
        //
        spas.assign (postponed_alts.begin (), postponed_alts.end ());

        std::sort (spas.begin (), spas.end (),
                   [] (build_package* x, build_package* y)
                   {
                     size_t xt (x->available->dependencies.size () -
                                x->dependencies->size ());

                     size_t yt (y->available->dependencies.size () -
                                y->dependencies->size ());

                     if (xt != yt)
                       return xt > yt ? -1 : 1;

                     // Also factor the package name and configuration path
                     // into the ordering to achieve a stable result.
                     //
                     int r (x->name ().compare (y->name ()));
                     return r != 0
                       ? r
                       : x->db.get ().config.compare (y->db.get ().config);
                   });

        // Calculate the maximum number of the enabled dependency
        // alternatives.
        //
        size_t max_enabled_count (0);

        for (build_package* p: spas)
        {
          assert (p->postponed_dependency_alternatives);

          size_t n (p->postponed_dependency_alternatives->size ());

          if (max_enabled_count < n)
            max_enabled_count = n;
        }

        assert (max_enabled_count != 0); // Wouldn't be here otherwise.

        // Try to select a dependency alternative with the lowest index,
        // preferring postponed packages with the longer tail of unprocessed
        // dependencies (see above for the reasoning).
        //
        for (size_t i (1); i <= max_enabled_count && !prog; ++i)
        {
          for (build_package* p: spas)
          {
            prs.clear ();
            pas.clear ();

            size_t ndep (p->dependencies->size ());

            build_package_refs dep_chain;

            l5 ([&]{trace << "index " << i << " collect alt-postponed "
                          << p->available_name_version_db ();});

            collect_build_prerequisites (o,
                                         *p,
                                         fdb,
                                         rpt_depts,
                                         apc,
                                         false /* initial_collection */,
                                         replaced_vers,
                                         dep_chain,
                                         &prs,
                                         &pas,
                                         i,
                                         postponed_deps,
                                         postponed_cfgs,
                                         postponed_poss,
                                         unacceptable_alts);

            prog = (pas.find (p) == pas.end () ||
                    ndep != p->dependencies->size ());

            // Save the potential new postponements.
            //
            if (prog)
            {
              postponed_alts.erase (p);
              postponed_alts.insert (pas.begin (), pas.end ());
            }

            size_t npr (postponed_repo.size ());
            postponed_repo.insert (prs.begin (), prs.end ());

            // Note that not collecting any alternative-relative postponements
            // but producing new repository-related postponements is progress
            // nevertheless.
            //
            // Note that we don't need to check for new configuration- related
            // postponements here since if they are present, then this package
            // wouldn't be in pas and so prog would be true (see above for
            // details).
            //
            if (!prog)
              prog = (npr != postponed_repo.size ());

            if (prog)
              break;
          }
        }

        if (prog)
          continue;
      }

      assert (!prog);

      // If we still have any non-negotiated clusters and non-replace
      // postponed positions, then it's possible one of them is the cross-
      // dependent pathological case where we will never hit it unless we
      // force the re-evaluation to earlier position (similar to the
      // single-dependent case, which we handle accurately). For example:
      //
      // tex: depends: libbar(c)
      //      depends: libfoo(c)
      //
      // tix: depends: libbar(c)
      //      depends: tex(c)
      //
      // Here tex and tix are existing dependent and we are upgrading tex.
      //
      // While it would be ideal to handle such cases accurately, it's not
      // trivial. So for now we resort to the following heuristics: when left
      // with no other option, we treat the first encountered non- replace
      // position as replace and see if that helps move things forward.
      //
      if (!postponed_cfgs.negotiated () &&
          find_if (postponed_poss.begin (), postponed_poss.end (),
                   [] (const auto& v) {return !v.second.replace;}) !=
          postponed_poss.end ()         &&
          !postponed_poss.replace)
      {
        l5 ([&]{trace << "non-negotiated clusters left and non-replace "
                      << "postponed positions are present, overriding first "
                      << "encountered non-replace position to replace";});

        postponed_poss.replace = true;
        prog = true;
        continue; // Go back to negotiating skipped cluster.
      }

      // Finally, erase the bogus postponements and re-collect from scratch,
      // if any (see postponed_dependencies for details).
      //
      // Note that we used to re-collect such postponements in-place but
      // re-doing from scratch feels more correct (i.e., we may end up doing
      // it earlier which will affect dependency alternatives).
      //
      postponed_deps.cancel_bogus (trace, false /* initial_collection */);
    }

    // Check if any negotiatiated configurations ended up with any bogus
    // variables (see retry_configuration catch block for background).
    //
    // Note that we could potentially end up yo-yo'ing: we remove a bogus and
    // that causes the original dependent to get re-visited which in turn
    // re-introduces the bogus. In other words, one of the bogus variables
    // which we have removed are actually the cause of no longer needing the
    // dependent that introduced it. Feels like the correct outcome of this
    // should be keeping the bogus variable that triggered yo-yo'ing. Of
    // course, there could be some that we should keep and some that we should
    // drop and figuring this out would require retrying all possible
    // combinations. An alternative solution would be to detect yo-yo'ing,
    // print the bogus variables involved, and ask the user to choose (with an
    // override) which ones to keep. Let's go with this for now.
    //
    {
      // On the first pass see if we have anything bogus.
      //
      bool bogus (false);
      for (postponed_configuration& pcfg: postponed_cfgs)
      {
        if (pcfg.negotiated && *pcfg.negotiated) // Negotiated.
        {
          for (package_configuration& cfg: pcfg.dependency_configurations)
          {
            for (config_variable_value& v: cfg)
            {
              if (v.dependent && !v.confirmed)
              {
                bogus = true;
                break;
              }
            }
            if (bogus) break;
          }
          if (bogus) break;
        }
      }

      if (bogus)
      {
        // On the second pass calculate the checksum of all the negotiated
        // clusters.
        //
        sha256 cs;
        for (postponed_configuration& pcfg: postponed_cfgs)
        {
          if (pcfg.negotiated && *pcfg.negotiated)
          {
            for (package_configuration& cfg: pcfg.dependency_configurations)
            {
              for (config_variable_value& v: cfg)
              {
                if (v.dependent)
                  to_checksum (cs, v);
              }
            }
          }
        }

        bool cycle;
        {
          string s (cs.string ());
          if (find (postponed_cfgs_history.begin (),
                    postponed_cfgs_history.end (),
                    s) == postponed_cfgs_history.end ())
          {
            postponed_cfgs_history.push_back (move (s));
            cycle = false;
          }
          else
            cycle = true;
        }

        // On the third pass we either retry or diagnose.
        //
        diag_record dr;
        if (cycle)
        {
          dr <<
            fail << "unable to remove bogus configuration values without "
                 << "causing configuration refinement cycle" <<
            info << "consider manually specifying one or more of the "
                 << "following variables as user configuration";
        }

        for (postponed_configuration& pcfg: postponed_cfgs)
        {
          optional<package_key> dept; // Bogus dependent.

          if (pcfg.negotiated && *pcfg.negotiated)
          {
            for (package_configuration& cfg: pcfg.dependency_configurations)
            {
              // Note that the entire dependency configuration may end up
              // being "bogus" (i.e., it does not contain any configuration
              // variables with a confirmed dependent). But that will be
              // handled naturally: we will either no longer have this
              // dependency in the cluster and thus never call its skeleton's
              // dependent_config() or this call will be no-op since it won't
              // find any dependent variables.
              //
              for (config_variable_value& v: cfg)
              {
                if (v.dependent && !v.confirmed)
                {
                  if (!dept)
                    dept = move (v.dependent);

                  if (cycle)
                    dr << "\n    " << v.serialize_cmdline ();
                  else
                    v.undefine ();
                }
              }
            }

            if (dept)
            {
              if (cycle)
                break;
              else
                throw retry_configuration {pcfg.depth, move (*dept)};
            }
          }

          if (dept)
            break;
        }
      }
    }

    // If any postponed_{repo,alts} builds remained, then perform the
    // diagnostics run. Naturally we shouldn't have any postponed_cfgs without
    // one of the former.
    //
    if (!postponed_repo.empty ())
    {
      build_package_refs dep_chain;

      collect_build_prerequisites (o,
                                   **postponed_repo.begin (),
                                   fdb,
                                   rpt_depts,
                                   apc,
                                   false /* initial_collection */,
                                   replaced_vers,
                                   dep_chain,
                                   nullptr,
                                   nullptr,
                                   0,
                                   postponed_deps,
                                   postponed_cfgs,
                                   postponed_poss,
                                   unacceptable_alts);

      assert (false); // Can't be here.
    }

    if (!postponed_alts.empty ())
    {
      build_package_refs dep_chain;

      collect_build_prerequisites (o,
                                   **postponed_alts.begin (),
                                   fdb,
                                   rpt_depts,
                                   apc,
                                   false /* initial_collection */,
                                   replaced_vers,
                                   dep_chain,
                                   nullptr,
                                   nullptr,
                                   0,
                                   postponed_deps,
                                   postponed_cfgs,
                                   postponed_poss,
                                   unacceptable_alts);

      assert (false); // Can't be here.
    }

    // While the assumption is that we shouldn't leave any non-negotiated
    // clusters, we can potentially miss some corner cases in the above "skip
    // configuration" logic. Let's thus trace the non-negotiated clusters
    // before the assertion.
    //
#ifndef NDEBUG
    for (const postponed_configuration& cfg: postponed_cfgs)
    {
      if (!cfg.negotiated || !*cfg.negotiated)
        trace << "unexpected non-negotiated cluster " << cfg;
    }

    assert (postponed_cfgs.negotiated ());
#endif

    l5 ([&]{trace << "end" << trace_suffix;});
  }

  build_packages::iterator build_packages::
  order (database& db,
         const package_name& name,
         optional<bool> buildtime,
         const function<find_database_function>& fdb,
         bool reorder)
  {
    package_refs chain;
    return order (db, name, buildtime, chain, fdb, reorder);
  }

  void build_packages::
  collect_order_dependents (const repointed_dependents& rpt_depts)
  {
    // For each package on the list we want to insert all its dependents
    // before it so that they get configured after the package on which they
    // depend is configured (remember, our build order is reverse, with the
    // last package being built first). This applies to both packages that are
    // already on the list as well as the ones that we add, recursively.
    //
    for (auto i (begin ()); i != end (); ++i)
    {
      const build_package& p (*i);

      // Prune if this is not a configured package being up/down-graded
      // or reconfigured.
      //
      assert (p.action);

      // Dropped package may have no dependents.
      //
      if (*p.action != build_package::drop && p.reconfigure ())
        collect_order_dependents (i, rpt_depts);
    }
  }

  void build_packages::
  collect_order_dependents (iterator pos,
                            const repointed_dependents& rpt_depts)
  {
    tracer trace ("collect_order_dependents");

    assert (pos != end ());

    build_package& p (*pos);

    database& pdb (p.db);
    const shared_ptr<selected_package>& sp (p.selected);

    const package_name& n (sp->name);

    // See if we are up/downgrading this package. In particular, the available
    // package could be NULL meaning we are just adjusting.
    //
    int ud (p.available != nullptr
            ? sp->version.compare (p.available_version ())
            : 0);

    for (database& ddb: pdb.dependent_configs ())
    {
      for (auto& pd: query_dependents_cache (ddb, n, pdb))
      {
        package_name& dn (pd.name);
        auto i (map_.find (ddb, dn));

        // Make sure the up/downgraded package still satisfies this
        // dependent. But first "prune" if the dependent is being dropped or
        // this is a replaced prerequisite of the repointed dependent.
        //
        // Note that the repointed dependents are always collected and have
        // all their collected prerequisites ordered (including new and old
        // ones). See collect_build_prerequisites() and order() for details.
        //
        bool check (ud != 0 && pd.constraint);

        if (i != map_.end () && i->second.position != end ())
        {
          build_package& dp (i->second.package);

          // Skip the droped dependent.
          //
          if (dp.action && *dp.action == build_package::drop)
            continue;

          repointed_dependents::const_iterator j (
            rpt_depts.find (package_key {ddb, dn}));

          if (j != rpt_depts.end ())
          {
            const map<package_key, bool>& prereqs_flags (j->second);

            auto k (prereqs_flags.find (package_key {pdb, n}));

            if (k != prereqs_flags.end () && !k->second)
              continue;
          }

          // There is one tricky aspect: the dependent could be in the process
          // of being reconfigured or up/downgraded as well. In this case all
          // we need to do is detect this situation and skip the test since
          // all the (new) constraints of this package have been satisfied in
          // collect_build().
          //
          if (check)
            check = !dp.dependencies;
        }

        if (check)
        {
          const version& av (p.available_version ());
          const version_constraint& c (*pd.constraint);

          if (!satisfies (av, c))
          {
            diag_record dr (fail);

            dr << "unable to " << (ud < 0 ? "up" : "down") << "grade "
               << "package " << *sp << pdb << " to ";

            // Print both (old and new) package names in full if the system
            // attribution changes.
            //
            if (p.system != sp->system ())
              dr << p.available_name_version ();
            else
              dr << av; // Can't be the wildcard otherwise would satisfy.

            dr << info << "because package " << dn << ddb << " depends on ("
               << n << " " << c << ")";

            string rb;
            if (!p.user_selection ())
            {
              for (const package_key& pk: p.required_by)
                rb += (rb.empty () ? " " : ", ") + pk.string ();
            }

            if (!rb.empty ())
              dr << info << "package " << p.available_name_version ()
                 << " required by" << rb;

            dr << info << "explicitly request up/downgrade of package "
               << dn;

            dr << info << "or explicitly specify package " << n
               << " version to manually satisfy these constraints";
          }

          // Add this contraint to the list for completeness.
          //
          p.constraints.emplace_back (ddb, dn.string (), c);
        }

        auto adjustment = [&dn, &ddb, &n, &pdb] () -> build_package
        {
          shared_ptr<selected_package> dsp (ddb.load<selected_package> (dn));

          // A system package cannot be a dependent.
          //
          assert (!dsp->system ());

          return build_package {
            build_package::adjust,
              ddb,
              move (dsp),
              nullptr,                   // No available pkg/repo fragment.
              nullptr,
              nullopt,                   // Dependencies.
              nullopt,                   // Dependencies alternatives.
              nullopt,                   // Package skeleton.
              nullopt,                   // Postponed dependency alternatives.
              false,                     // Recursive collection.
              nullopt,                   // Hold package.
              nullopt,                   // Hold version.
              {},                        // Constraints.
              false,                     // System.
              false,                     // Keep output directory.
              false,                     // Disfigure (from-scratch reconf).
              false,                     // Configure-only.
              nullopt,                   // Checkout root.
              false,                     // Checkout purge.
              strings (),                // Configuration variables.
              {package_key {pdb, n}},    // Required by (dependency).
              false,                     // Required by dependents.
              build_package::adjust_reconfigure};
        };

        // We can have three cases here: the package is already on the list,
        // the package is in the map (but not on the list) and it is in
        // neither.
        //
        // If the existing entry is pre-entered, is an adjustment, or is a
        // build that is not supposed to be built (not in the list), then we
        // merge it into the new adjustment entry. Otherwise (is a build in
        // the list), we just add the reconfigure adjustment flag to it.
        //
        if (i != map_.end ())
        {
          build_package& dp (i->second.package);
          iterator& dpos (i->second.position);

          if (!dp.action                         || // Pre-entered.
              *dp.action != build_package::build || // Non-build.
              dpos == end ())                       // Build not in the list.
          {
            build_package bp (adjustment ());
            bp.merge (move (dp));
            dp = move (bp);
          }
          else                                       // Build in the list.
            dp.flags |= build_package::adjust_reconfigure;

          // It may happen that the dependent is already in the list but is
          // not properly ordered against its dependencies that get into the
          // list via another dependency path. Thus, we check if the dependent
          // is to the right of its dependency and, if that's the case,
          // reinsert it in front of the dependency.
          //
          if (dpos != end ())
          {
            for (auto i (pos); i != end (); ++i)
            {
              if (i == dpos)
              {
                erase (dpos);
                dpos = insert (pos, dp);
                break;
              }
            }
          }
          else
            dpos = insert (pos, dp);
        }
        else
        {
          // Don't move dn since it is used by adjustment().
          //
          i = map_.emplace (package_key {ddb, dn},
                            data_type {end (), adjustment ()}).first;

          i->second.position = insert (pos, i->second.package);
        }

        // Recursively collect our own dependents inserting them before us.
        //
        // Note that we cannot end up with an infinite recursion for
        // configured packages due to a dependency cycle (see order() for
        // details).
        //
        collect_order_dependents (i->second.position, rpt_depts);
      }
    }
  }

  void build_packages::
  clear ()
  {
    build_package_list::clear ();
    map_.clear ();
  }

  void build_packages::
  clear_order ()
  {
    build_package_list::clear ();

    for (auto& p: map_)
      p.second.position = end ();
  }

  void build_packages::
  verify_ordering () const
  {
    for (const auto& b: map_)
    {
      const build_package& bp (b.second.package);

      auto i (find_if (begin (), end (),
                       [&bp] (const build_package& p) {return &p == &bp;}));

      // List ordering must properly be reflected in the tree entries.
      //
      assert (i == b.second.position);

      // Pre-entered builds must never be ordered and the real build actions
      // (builds, adjustments, etc) must all be ordered.
      //
      // Note that the later was not the case until we've implemented
      // re-collection from scratch after the package version replacement (see
      // replaced_versions for details). Before that the whole dependency
      // trees from the being replaced dependent stayed in the map.
      //
      assert (bp.action.has_value () == (i != end ()));
    }
  }

  vector<build_packages::existing_dependent> build_packages::
  query_existing_dependents (
    tracer& trace,
    database& db,
    const package_name& name,
    const replaced_versions& replaced_vers,
    const repointed_dependents& rpt_depts,
    const function<verify_dependent_build_function>& vdb)
  {
    vector<existing_dependent> r;

    lazy_shared_ptr<selected_package> sp (db, name);

    for (database& ddb: db.dependent_configs ())
    {
      for (auto& pd: query_dependents (ddb, name, db))
      {
        shared_ptr<selected_package> dsp (
          ddb.load<selected_package> (pd.name));

        auto i (dsp->prerequisites.find (sp));
        assert (i != dsp->prerequisites.end ());

        const auto& pos (i->second.config_position);

        if (pos.first != 0) // Has config clause?
        {
          package_key pk (ddb, pd.name);

          if (rpt_depts.find (pk) != rpt_depts.end ())
          {
            l5 ([&]{trace << "skip repointed existing dependent " << pk
                          << " of dependency " << name << db;});
            continue;
          }

          // Ignore dependent which is already being built or dropped.
          //
          const build_package* p (entered_build (pk));

          if (p != nullptr && p->action)
          {
            bool build;
            if (((build = *p->action == build_package::build) &&
                 (p->system || p->recollect_recursively (rpt_depts))) ||
                *p->action == build_package::drop)
            {
              if (!build || !vdb || !vdb (pk, pos))
              {
                l5 ([&]{trace << "skip being "
                              << (build ? "built" : "dropped")
                              << " existing dependent " << pk
                              << " of dependency " << name << db;});
                continue;
              }
            }
          }

          // Ignore dependent which is expected to be built or dropped.
          //
          auto vi (replaced_vers.find (pk));
          if (vi != replaced_vers.end () && !vi->second.replaced)
          {
            bool build (vi->second.available != nullptr);

            l5 ([&]{trace << "skip expected to be "
                          << (build ? "built" : "dropped")
                          << " existing dependent " << pk
                          << " of dependency " << name << db;});

            continue;
          }

          r.push_back (existing_dependent {ddb, move (dsp), pos});
        }
      }
    }

    return r;
  }

  const build_package* build_packages::
  replace_existing_dependent_dependency (
    tracer& trace,
    const pkg_build_options& o,
    existing_dependent& ed,
    pair<size_t, size_t> pos,
    const function<find_database_function>& fdb,
    const repointed_dependents& rpt_depts,
    const function<add_priv_cfg_function>& apc,
    bool initial_collection,
    replaced_versions& replaced_vers,
    postponed_configurations& postponed_cfgs)
  {
    // The repointed dependent cannot be returned by
    // query_existing_dependents(). Note that the repointed dependent
    // references both old and new prerequisites.
    //
    assert (rpt_depts.find (package_key (ed.db, ed.selected->name)) ==
            rpt_depts.end ());

    shared_ptr<selected_package> dsp;
    database* pdb (nullptr);
    const version_constraint* vc (nullptr);

    // Find the dependency for this earlier dependency position. We know it
    // must be there since it's with configuration.
    //
    for (const auto& p: ed.selected->prerequisites)
    {
      if (p.second.config_position == pos)
      {
        pdb = &p.first.database ();

        dsp = p.first.load ();

        l5 ([&]{trace << "replace dependency at index "
                      << ed.dependency_position.first
                      << " of existing dependent " << *ed.selected
                      << ed.db << " with dependency " << *dsp
                      << *pdb << " at index " << pos.first;});

        if (p.second.constraint)
          vc = &*p.second.constraint;
      }
    }

    assert (dsp != nullptr);

    package_key pk (*pdb, dsp->name);

    // Adjust the existing dependent entry.
    //
    ed.dependency_position = pos;

    // Collect the package build for this dependency.
    //
    pair<shared_ptr<available_package>,
         lazy_shared_ptr<repository_fragment>> rp (
           find_available_fragment (o, pk.db, dsp));

    bool system (dsp->system ());

    package_key dpk (ed.db, ed.selected->name);

    build_package p {
      build_package::build,
        pk.db,
        move (dsp),
        move (rp.first),
        move (rp.second),
        nullopt,                    // Dependencies.
        nullopt,                    // Dependencies alternatives.
        nullopt,                    // Package skeleton.
        nullopt,                    // Postponed dependency alternatives.
        false,                      // Recursive collection.
        nullopt,                    // Hold package.
        nullopt,                    // Hold version.
        {},                         // Constraints.
        system,                     // System.
        false,                      // Keep output directory.
        false,                      // Disfigure (from-scratch reconf).
        false,                      // Configure-only.
        nullopt,                    // Checkout root.
        false,                      // Checkout purge.
        strings (),                 // Configuration variables.
        {dpk},                      // Required by (dependent).
        true,                       // Required by dependents.
        build_package::adjust_reconfigure};

    if (vc != nullptr)
      p.constraints.emplace_back (dpk.db, dpk.name.string (), *vc);

    // Note: not recursive.
    //
    collect_build (o,
                   move (p),
                   fdb,
                   rpt_depts,
                   apc,
                   initial_collection,
                   replaced_vers,
                   postponed_cfgs);

    return entered_build (pk);
  }

  build_packages::iterator build_packages::
  order (database& db,
         const package_name& name,
         optional<bool> buildtime,
         package_refs& chain,
         const function<find_database_function>& fdb,
         bool reorder)
  {
    package_map::iterator mi;

    if (buildtime)
    {
      database* ddb (fdb (db, name, *buildtime));

      mi = ddb != nullptr
        ? map_.find (*ddb, name)
        : map_.find_dependency (db, name, *buildtime);
    }
    else
      mi = map_.find (db, name);

    // Every package that we order should have already been collected.
    //
    assert (mi != map_.end ());

    build_package& p (mi->second.package);

    assert (p.action); // Can't order just a pre-entered package.

    database& pdb (p.db);

    // Make sure there is no dependency cycle.
    //
    package_ref cp {pdb, name};
    {
      auto i (find (chain.begin (), chain.end (), cp));

      if (i != chain.end ())
      {
        diag_record dr (fail);
        dr << "dependency cycle detected involving package " << name << pdb;

        auto nv = [this] (const package_ref& cp)
        {
          auto mi (map_.find (cp.db, cp.name));
          assert (mi != map_.end ());

          build_package& p (mi->second.package);

          assert (p.action); // See above.

          // We cannot end up with a dependency cycle for actions other than
          // build since these packages are configured and we would fail on a
          // previous run while building them.
          //
          assert (p.available != nullptr);

          return p.available_name_version_db ();
        };

        // Note: push_back() can invalidate the iterator.
        //
        size_t j (i - chain.begin ());

        for (chain.push_back (cp); j != chain.size () - 1; ++j)
          dr << info << nv (chain[j]) << " depends on " << nv (chain[j + 1]);
      }
    }

    // If this package is already in the list, then that would also mean all
    // its prerequisites are in the list and we can just return its
    // position. Unless we want it reordered.
    //
    iterator& pos (mi->second.position);
    if (pos != end ())
    {
      if (reorder)
        erase (pos);
      else
        return pos;
    }

    // Order all the prerequisites of this package and compute the position of
    // its "earliest" prerequisite -- this is where it will be inserted.
    //
    const shared_ptr<selected_package>&  sp (p.selected);
    const shared_ptr<available_package>& ap (p.available);

    bool build (*p.action == build_package::build);

    // Package build must always have the available package associated.
    //
    assert (!build || ap != nullptr);

    // Unless this package needs something to be before it, add it to the end
    // of the list.
    //
    iterator i (end ());

    // Figure out if j is before i, in which case set i to j. The goal here is
    // to find the position of our "earliest" prerequisite.
    //
    auto update = [this, &i] (iterator j)
    {
      for (iterator k (j); i != j && k != end ();)
        if (++k == i)
          i = j;
    };

    // Similar to collect_build(), we can prune if the package is already
    // configured, right? While in collect_build() we didn't need to add
    // prerequisites of such a package, it doesn't mean that they actually
    // never ended up in the map via another dependency path. For example,
    // some can be a part of the initial selection. And in that case we must
    // order things properly.
    //
    // Also, if the package we are ordering is not a system one and needs to
    // be disfigured during the plan execution, then we must order its
    // (current) dependencies that also need to be disfigured.
    //
    // And yet, if the package we are ordering is a repointed dependent, then
    // we must order not only its unamended and new prerequisites but also its
    // replaced prerequisites, which can also be disfigured.
    //
    bool src_conf (sp != nullptr &&
                   sp->state == package_state::configured &&
                   sp->substate != package_substate::system);

    auto disfigure = [] (const build_package& p)
    {
      return p.action && (*p.action == build_package::drop || p.reconfigure ());
    };

    bool order_disfigured (src_conf && disfigure (p));

    chain.push_back (cp);

    // Order the build dependencies.
    //
    if (build && !p.system)
    {
      // So here we are going to do things differently depending on whether
      // the package is already configured or not. If it is and not as a
      // system package, then that means we can use its prerequisites
      // list. Otherwise, we use the manifest data.
      //
      if (src_conf                              &&
          sp->version == p.available_version () &&
          (p.config_vars.empty () ||
           !has_buildfile_clause (ap->dependencies)))
      {
        for (const auto& p: sp->prerequisites)
        {
          database& db (p.first.database ());
          const package_name& name (p.first.object_id ());

          // The prerequisites may not necessarily be in the map.
          //
          // Note that for the repointed dependent we also order its new and
          // replaced prerequisites here, since they all are in the selected
          // package prerequisites set.
          //
          auto i (map_.find (db, name));
          if (i != map_.end () && i->second.package.action)
            update (order (db,
                           name,
                           nullopt /* buildtime */,
                           chain,
                           fdb,
                           false   /* reorder */));
        }

        // We just ordered them among other prerequisites.
        //
        order_disfigured = false;
      }
      else
      {
        // The package prerequisites builds must already be collected and
        // thus the resulting dependency list is complete.
        //
        assert (p.dependencies &&
                p.dependencies->size () == ap->dependencies.size ());

        // We are iterating in reverse so that when we iterate over the
        // dependency list (also in reverse), prerequisites will be built in
        // the order that is as close to the manifest as possible.
        //
        for (const dependency_alternatives_ex& das:
               reverse_iterate (*p.dependencies))
        {
          // The specific dependency alternative must already be selected,
          // unless this is a toolchain build-time dependency or all the
          // alternatives are disabled in which case the alternatives list
          // is empty.
          //
          if (das.empty ())
            continue;

          assert (das.size () == 1);

          for (const dependency& d: das.front ())
          {
            // Note that for the repointed dependent we only order its new and
            // unamended prerequisites here. Its replaced prerequisites will
            // be ordered below.
            //
            update (order (pdb,
                           d.name,
                           das.buildtime,
                           chain,
                           fdb,
                           false /* reorder */));
          }
        }
      }
    }

    // Order the dependencies being disfigured.
    //
    if (order_disfigured)
    {
      for (const auto& p: sp->prerequisites)
      {
        database& db (p.first.database ());
        const package_name& name (p.first.object_id ());

        // The prerequisites may not necessarily be in the map.
        //
        auto i (map_.find (db, name));

        // Note that for the repointed dependent we also order its replaced
        // and potentially new prerequisites here (see above). The latter is
        // redundant (we may have already ordered them above) but harmless,
        // since we do not reorder.
        //
        if (i != map_.end () && disfigure (i->second.package))
          update (order (db,
                         name,
                         nullopt /* buildtime */,
                         chain,
                         fdb,
                         false   /* reorder */));
      }
    }

    chain.pop_back ();

    return pos = insert (i, p);
  }

  build_packages::package_map::iterator build_packages::package_map::
  find_dependency (database& db, const package_name& pn, bool buildtime)
  {
    iterator r (end ());

    linked_databases ldbs (db.dependency_configs (pn, buildtime));

    for (database& ldb: ldbs)
    {
      iterator i (find (ldb, pn));
      if (i != end ())
      {
        if (r == end ())
          r = i;
        else
          fail << "building package " << pn << " in multiple "
               << "configurations" <<
            info << r->first.db.get().config_orig <<
            info << ldb.config_orig <<
            info << "use --config-* to select package configuration";
      }
    }

    return r;
  }
}
