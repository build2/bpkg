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
#include <bpkg/rep-mask.hxx>
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
  const system_package_status* build_package::
  system_status () const
  {
    assert (action);

    if (*action != build_package::drop && system)
    {
      const optional<system_repository>& sys_rep (db.get ().system_repository);
      assert (sys_rep);

      if (const system_package* sys_pkg = sys_rep->find (name ()))
        return sys_pkg->system_status;
    }

    return nullptr;
  }

  const system_package_status* build_package::
  system_install () const
  {
    if (const system_package_status* s = system_status ())
      return s->status == system_package_status::partially_installed ||
             s->status == system_package_status::not_installed
             ? s
             : nullptr;

    return nullptr;
  }

  bool build_package::
  user_selection () const
  {
    return required_by.find (package_version_key (db.get ().main_database (),
                                                  "command line")) !=
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
            (flags & build_recollect) != 0                   ||
            ((!config_vars.empty () || skeleton) &&
             has_buildfile_clause (available->dependencies)) ||
            rpt_depts.find (package_key (db, name ())) != rpt_depts.end ());
  }

  bool build_package::
  recursive_collection_postponed () const
  {
    assert (action && *action == build_package::build && available != nullptr);

    return dependencies &&
           dependencies->size () != available->dependencies.size ();
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
              replace ()                                ||
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
    // Note that in the common case a package version doesn't come from too
    // many repositories (8).
    //
    small_vector<reference_wrapper<const package_location>, 8> locations;

    if (available != nullptr) // Not adjustment?
    {
      locations.reserve (available->locations.size ());

      for (const package_location& pl: available->locations)
      {
        if (!rep_masked_fragment (pl.repository_fragment))
          locations.push_back (pl);
      }
    }

    if (locations.empty ())
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
      const package_location& pl (locations[0]);

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
        for (const package_location& pl: locations)
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

    // If true, then add the user-selection tag.
    //
    bool add_user_selection (false);

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
      add_user_selection = true;
    }

    // Merge in the required-by package names only if semantics matches.
    // Otherwise, prefer the "required by dependents" semantics since we, in
    // particular, should never replace such package builds in the map with
    // package drops (see collect_drop() for details).
    //
    if (p.required_by_dependents == required_by_dependents)
    {
      required_by.insert (p.required_by.begin (), p.required_by.end ());
    }
    else if (p.required_by_dependents)
    {
      // Restore the user-selection tag.
      //
      if (user_selection ())
        add_user_selection = true;

      required_by_dependents = true;
      required_by = move (p.required_by);
    }

    if (add_user_selection)
      required_by.emplace (db.get ().main_database (), "command line");

    // Copy constraints, suppressing duplicates.
    //
    if (!constraints.empty ())
    {
      for (constraint_type& c: p.constraints)
      {
        if (find_if (constraints.begin (), constraints.end (),
                     [&c] (const constraint_type& v)
                     {
                       return v.dependent == c.dependent && v.value == c.value;
                     }) == constraints.end ())
        {
          constraints.push_back (move (c));
        }
      }
    }
    else
      constraints = move (p.constraints);

    // Copy upgrade flag if "stronger" (existing wins over non-existing and
    // upgrade wins over patch).
    //
    if (upgrade < p.upgrade)
      upgrade = p.upgrade;

    // Copy deorphan flag if greater.
    //
    if (p.deorphan)
      deorphan = true;

    // Copy hold_* flags if they are "stronger".
    //
    if (!hold_package || (p.hold_package && *p.hold_package > *hold_package))
      hold_package = p.hold_package;

    if (!hold_version || (p.hold_version && *p.hold_version > *hold_version))
      hold_version = p.hold_version;

    // Copy state flags and upgrade dependent repointments and re-evaluations
    // to the full builds. But in contrast to the repointed dependents we may
    // merge two dependent re-evaluations.
    //
    flags |= (p.flags & ~build_reevaluate);

    if (*action == build)
    {
      flags &= ~build_repoint;

      if ((p.flags & build_reevaluate) == 0)
        flags &= ~build_reevaluate;
    }

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
                 bool load_old_dependent_config,
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

    optional<dir_path> src_root;
    optional<dir_path> out_root;

    optional<dir_path> old_src_root;
    optional<dir_path> old_out_root;
    uint16_t load_config_flags (0);

    if (ap != nullptr)
    {
      bool src_conf (selected != nullptr                          &&
                     selected->state == package_state::configured &&
                     selected->substate != package_substate::system);

      database& pdb (db);

      // If the package is being reconfigured, then specify {src,out}_root as
      // the existing source and output root directories not to create the
      // skeleton directory needlessly. Otherwise, if the being built package
      // is external, then specify src_root as its existing source directory
      // and out_root as its potentially non-existing output directory.
      //
      // Can we actually use the existing output root directory if the package
      // is being reconfigured but we are requested to ignore the current
      // configuration? Yes we can, since load_config_flags stays 0 in this
      // case and all the variables in config.build will be ignored.
      //
      if (src_conf && ap->version == selected->version)
      {
        src_root = selected->effective_src_root (pdb.config);
        out_root = selected->effective_out_root (pdb.config);
      }
      else
      {
        src_root = external_dir ();

        if (src_root)
          out_root = dir_path (pdb.config) /= name ().string ();
      }

      // Specify old_{src,out}_root paths and set load_config_flags if the old
      // configuration is present and is requested to be loaded.
      //
      if (src_conf && (!disfigure || load_old_dependent_config))
      {
        old_src_root = selected->effective_src_root (pdb.config);
        old_out_root = selected->effective_out_root (pdb.config);

        if (!disfigure)
          load_config_flags |= package_skeleton::load_config_user;

        if (load_old_dependent_config)
          load_config_flags |= package_skeleton::load_config_dependent;
      }
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
      move (out_root),
      move (old_src_root),
      move (old_out_root),
      load_config_flags);

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

  // unsatisfied_dependents
  //
  void unsatisfied_dependents::
  add (const package_key& dpt,
       const package_key& dep,
       const version_constraint& c,
       vector<unsatisfied_constraint>&& ucs,
       vector<package_key>&& dc)
  {
    if (unsatisfied_dependent* ud = find_dependent (dpt))
    {
      vector<ignored_constraint>& ics (ud->ignored_constraints);

      // Skip the dependency if it is already in the list.
      //
      // It feels that it may already be present in the list with a different
      // constraint (think of multiple depends clauses with the same
      // dependency), in which case we leave it unchanged.
      //
      if (find_if (ics.begin (), ics.end (),
                   [dep] (const auto& v) {return v.dependency == dep;}) ==
          ics.end ())
      {
        ics.emplace_back (dep, c, move (ucs), move (dc));
      }
    }
    else
      push_back (
        unsatisfied_dependent {
          dpt, {ignored_constraint (dep, c, move (ucs), move (dc))}});
  }

  unsatisfied_dependent* unsatisfied_dependents::
  find_dependent (const package_key& dk)
  {
    auto i (find_if (begin (), end (),
                     [&dk] (const unsatisfied_dependent& v)
                     {
                       return dk == v.dependent;
                     }));
    return i != end () ? &*i : nullptr;
  }

  void unsatisfied_dependents::
  diag (const build_packages& pkgs)
  {
    assert (!empty ());

    const unsatisfied_dependent& dpt (front ());
    const package_key& dk (dpt.dependent);

    assert (!dpt.ignored_constraints.empty ());

    const ignored_constraint& ic (dpt.ignored_constraints.front ());

    const build_package* p (pkgs.entered_build (ic.dependency));
    assert (p != nullptr); // The dependency must be collected.

    const version_constraint& c (ic.constraint);
    const vector<unsatisfied_constraint>& ucs (ic.unsatisfied_constraints);

    const package_name& n (p->name ());

    //             "  info: ..."
    string indent ("          ");

    if (ucs.empty ()) // 'unable to up/downgrade package' failure.
    {
      database& pdb (p->db);
      const shared_ptr<selected_package>& sp (p->selected);

      // Otherwise, this would be a dependency adjustment (not an
      // up/down-grade), and thus the dependent must be satisfied with the
      // already configured dependency.
      //
      assert (p->available != nullptr);

      const version& av (p->available_version ());

      // See if we are upgrading or downgrading this package.
      //
      int ud (sp->version.compare (av));

      // Otherwise, the dependent must be satisfied with the already
      // configured dependency.
      //
      assert (ud != 0);

      diag_record dr (fail);
      dr << "unable to " << (ud < 0 ? "up" : "down") << "grade "
         << "package " << *sp << pdb << " to ";

      // Print both (old and new) package names in full if the system
      // attribution changes.
      //
      if (p->system != sp->system ())
        dr << p->available_name_version ();
      else
        dr << av; // Can't be the wildcard otherwise would satisfy.

      shared_ptr<selected_package> dsp (
        dk.db.get ().load<selected_package> (dk.name));

      assert (dsp != nullptr); // By definition.

      dr << info << "because configured package " << *dsp << dk.db
                 << " depends on (" << n << ' ' << c << ')';

      // Print the dependency constraints tree for this unsatisfied dependent,
      // which only contains constraints which come from its selected
      // dependents, recursively.
      //
      {
        set<package_key> printed;
        pkgs.print_constraints (
          dr,
          dk,
          indent,
          printed,
          (verb >= 2 ? optional<bool> () : true) /* selected_dependent */);
      }

      // If the dependency we failed to up/downgrade is not explicitly
      // specified on the command line, then print its dependency constraints
      // tree which only contains constraints which come from its being built
      // dependents, recursively.
      //
      if (!p->user_selection ())
      {
        // The dependency upgrade is always required by someone, the command
        // line or a package.
        //
        assert (!p->required_by.empty ());

        dr << info << "package " << p->available_name_version_db ();

        // Note that if the required_by member contains the dependencies,
        // rather than the dependents, we will subsequently print the
        // dependency constraints trees for these dependencies rather than a
        // single constraints tree (rooted in the dependency we failed to
        // up/downgrade). Also note that in this case we will still reuse the
        // same printed packages cache for all print_constraints() calls,
        // since it will likely be considered as a single dependency graph by
        // the user.
        //
        bool rbd (p->required_by_dependents);
        dr << (rbd ? " required by" : " dependent of");

        set<package_key> printed;
        for (const package_version_key& pvk: p->required_by)
        {
          dr << '\n' << indent << pvk;

          if (rbd)
          {
            const vector<build_package::constraint_type>& cs (p->constraints);
            auto i (find_if (cs.begin (), cs.end (),
                             [&pvk] (const build_package::constraint_type& v)
                             {
                               return v.dependent == pvk;
                             }));

            if (i != cs.end ())
              dr << " (" << p->name () << ' ' << i->value << ')';
          }

          indent += "  ";
          pkgs.print_constraints (
            dr,
            package_key (pvk.db, pvk.name),
            indent,
            printed,
            (verb >= 2 ? optional<bool> () : false) /* selected_dependent */);

          indent.resize (indent.size () - 2);
        }
      }

      if (verb < 2)
        dr << info << "re-run with -v for additional dependency information";

      dr << info << "consider re-trying with --upgrade|-u potentially combined "
                 << "with --recursive|-r" <<
        info << "or explicitly request up/downgrade of package " << dk.name <<
        info << "or explicitly specify package " << n << " version to "
             << "manually satisfy these constraints" << endf;
    }
    else              // 'unable to satisfy constraints' failure.
    {
      diag_record dr (fail);
      dr << "unable to satisfy constraints on package " << n;

      for (const unsatisfied_constraint& uc: ucs)
      {
        const build_package::constraint_type& c (uc.constraint);

        dr << info << c.dependent << " depends on (" << n << ' ' << c.value
                   << ')';

        if (const build_package* d = pkgs.dependent_build (c))
        {
          set<package_key> printed;
          pkgs.print_constraints (dr, *d, indent, printed);
        }
      }

      for (const unsatisfied_constraint& uc: ucs)
      {
        dr << info << "available "
           << package_string (n, uc.available_version, uc.available_system);
      }

      for (const package_key& d: reverse_iterate (ic.dependency_chain))
      {
        const build_package* p (pkgs.entered_build (d));
        assert (p != nullptr);

        dr << info << "while satisfying " << p->available_name_version_db ();
      }

      dr << info << "explicitly specify " << n << " version to "
                 << "manually satisfy both constraints" << endf;
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
      for (package_key& p: dep)
      {
        // Add the dependency unless it's already there.
        //
        if (find (d->begin (), d->end (), p) == d->end ())
        {
          // Feels like we can accumulate new dependencies into an existing
          // position only for an existing dependent. Note that we could still
          // try to add an (supposedly) identical entry for a non-existent
          // dependent (via some murky paths). Feels like this should be
          // harmless.
          //
          assert (existing);

          d->push_back (move (p));
        }
      }

      // Set the has_alternative flag for an existing dependent. Note that
      // it shouldn't change if already set.
      //
      if (dep.has_alternative)
      {
        if (!d->has_alternative)
        {
          assert (existing); // As above.
          d->has_alternative = *dep.has_alternative;
        }
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

  void postponed_configuration::
  merge (postponed_configuration&& c)
  {
    assert (c.id != id); // Can't merge to itself.

    merged_ids.push_back (c.id);

    for (size_t mid: c.merged_ids)
      merged_ids.push_back (mid);

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
  is_shadow_cluster (const postponed_configuration& c)
  {
    if (shadow_cluster.size () != c.dependents.size ())
      return false;

    for (auto& dt: c.dependents)
    {
      auto i (shadow_cluster.find (dt.first));
      if (i == shadow_cluster.end ())
        return false;

      const positions& ps (i->second);

      if (ps.size () != dt.second.dependencies.size ())
        return false;

      for (auto& d: dt.second.dependencies)
      {
        if (find (ps.begin (), ps.end (), d.position) == ps.end ())
          return false;
      }
    }

    return true;
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

  const build_package* build_packages::
  dependent_build (const build_package::constraint_type& c) const
  {
    const build_package* r (nullptr);

    if (c.dependent.version)
    try
    {
      r = entered_build (c.dependent.db, c.dependent.name);
      assert (r != nullptr); // Expected to be collected.
    }
    catch (const invalid_argument&)
    {
      // Must be a package name since the version is specified.
      //
      assert (false);
    }

    return r;
  }

  void build_packages::
  enter (package_name name, build_package pkg)
  {
    assert (!pkg.action && pkg.repository_fragment == nullptr);

    database& db (pkg.db); // Save before the move() call.
    auto p (map_.emplace (package_key {db, move (name)},
                          data_type {end (), move (pkg)}));

    assert (p.second);
  }

  build_package* build_packages::
  collect_build (const pkg_build_options& options,
                 build_package pkg,
                 replaced_versions& replaced_vers,
                 postponed_configurations& postponed_cfgs,
                 unsatisfied_dependents& unsatisfied_depts,
                 build_package_refs* dep_chain,
                 const function<find_database_function>& fdb,
                 const function<add_priv_cfg_function>& apc,
                 const repointed_dependents* rpt_depts,
                 postponed_packages* postponed_repo,
                 postponed_packages* postponed_alts,
                 postponed_packages* postponed_recs,
                 postponed_existing_dependencies* postponed_edeps,
                 postponed_dependencies* postponed_deps,
                 unacceptable_alternatives* unacceptable_alts,
                 const function<verify_package_build_function>& vpb)
  {
    using std::swap; // ...and not list::swap().

    using constraint_type = build_package::constraint_type;

    tracer trace ("collect_build");

    assert (pkg.repository_fragment == nullptr ||
            !rep_masked_fragment (pkg.repository_fragment));

    // See the above notes.
    //
    bool recursive (fdb != nullptr);

    assert ((!recursive || dep_chain != nullptr)        &&
            (rpt_depts         != nullptr) == recursive &&
            (postponed_repo    != nullptr) == recursive &&
            (postponed_alts    != nullptr) == recursive &&
            (postponed_recs    != nullptr) == recursive &&
            (postponed_edeps   != nullptr) == recursive &&
            (postponed_deps    != nullptr) == recursive &&
            (unacceptable_alts != nullptr) == recursive);

    // Only builds are allowed here.
    //
    assert (pkg.action && *pkg.action == build_package::build &&
            pkg.available != nullptr);

    package_key pk (pkg.db, pkg.available->id.name);

    // Apply the version replacement, if requested, and indicate that it was
    // applied. Ignore the replacement if its version doesn't satisfy the
    // dependency constraints specified by the caller. Also ignore if this is
    // a drop and the required-by package names of the specified build package
    // object have the "required by dependents" semantics.
    //
    auto vi (replaced_vers.find (pk));
    const version* replacement_version (nullptr);

    if (vi != replaced_vers.end ())
    {
      replaced_version& v (vi->second);

      if (v.available != nullptr)
        replacement_version = (v.system
                               ? v.available->system_version (pk.db)
                               : &v.available->version);

      if (!vi->second.replaced)
      {
        l5 ([&]{trace << "apply version replacement for "
                      << pkg.available_name_version_db ();});

        if (v.available != nullptr)
        {
          assert (replacement_version != nullptr);

          const version& rv (*replacement_version);

          bool replace (true);
          for (const constraint_type& c: pkg.constraints)
          {
            if (!satisfies (rv, c.value))
            {
              replace = false;

              l5 ([&]{trace << "replacement to " << rv << " is denied since "
                            << c.dependent << " depends on (" << pk.name << ' '
                            << c.value << ')';});
            }
          }

          if (replace)
          {
            v.replaced = true;

            pkg.available = v.available;
            pkg.repository_fragment = v.repository_fragment;
            pkg.system = v.system;

            l5 ([&]{trace << "replacement: "
                          << pkg.available_name_version_db ();});
          }
        }
        else
        {
          if (!pkg.required_by_dependents)
          {
            v.replaced = true;

            l5 ([&]{trace << "replacement: drop";});

            // We shouldn't be replacing a package build with the drop if someone
            // depends on this package.
            //
            assert (pkg.selected != nullptr);

            collect_drop (options, pkg.db, pkg.selected, replaced_vers);
            return nullptr;
          }
          else
          {
            assert (!pkg.required_by.empty ());

            l5 ([&]
                {
                  diag_record dr (trace);
                  dr << "replacement to drop is denied since " << pk
                     << " is required by ";
                  for (auto b (pkg.required_by.begin ()), i (b);
                       i != pkg.required_by.end ();
                       ++i)
                    dr << (i != b ? ", " : "") << *i;
                });
          }
        }
      }
    }

    // Add the version replacement entry, call the verification function if
    // specified, and throw replace_version.
    //
    // Note that this package can potentially be present in the unsatisfied
    // dependents list on the dependency side with the replacement version
    // being unsatisfactory for the ignored constraint. In this case, during
    // the from-scratch re-collection this replacement will be ignored if/when
    // this package is collected with this constraint specified. But it can
    // still be applied for some later collect_build() call or potentially
    // turn out bogus.
    //
    auto replace_ver = [&pk, &vpb, &vi, &replaced_vers] (const build_package& p)
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

    // If we already have an entry for this package name, then we have to pick
    // one over the other.
    //
    // If the existing entry is a drop, then we override it. If the existing
    // entry is a pre-entered or is non-build one, then we merge it into the
    // new build entry. Otherwise (both are builds), we pick one and merge the
    // other into it.
    //
    if (i != map_.end ())
    {
      build_package& bp (i->second.package);

      // Note that we used to think that the scenario when the build could
      // replace drop could never happen since we would start collecting from
      // scratch. This has changed when we introduced replaced_versions for
      // collecting drops.
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
        // At the end we want p1 to point to the object that we keep and p2 to
        // the object that we merge from.
        //
        build_package* p1 (&bp);
        build_package* p2 (&pkg);

        // Pick with the following preference order: user selection over
        // implicit one, source package over a system one, replacement version
        // over a non-replacement one, newer version over an older one. So get
        // the preferred into p1 and the other into p2.
        //
        {
          const version& v1 (p1->available_version ());
          const version& v2 (p2->available_version ());

          int us (p1->user_selection () - p2->user_selection ());
          int sf (p1->system - p2->system);
          int rv (replacement_version != nullptr
                  ? (v1 == *replacement_version) - (v2 == *replacement_version)
                  : 0);

          if (us < 0              ||
              (us == 0 && sf > 0) ||
              (us == 0 &&
               sf == 0 &&
               (rv < 0 || (rv == 0 && v2 > v1))))
          {
            swap (p1, p2);
          }
        }

        // If the versions differ, pick the satisfactory one and if both are
        // satisfactory, then keep the preferred.
        //
        // If neither of the versions is satisfactory, then ignore those
        // unsatisfied constraints which prevent us from picking the package
        // version which is currently in the map. It feels that the version in
        // the map is no worse than the other one and we choose it
        // specifically for the sake of optimization, trying to avoid throwing
        // the replace_version exception.
        //
        if (p1->available_version () != p2->available_version ())
        {
          // See if pv's version satisfies pc's constraints, skipping those
          // which are meant to be ignored (ics). Return the pointer to the
          // unsatisfied constraint or NULL if all are satisfied.
          //
          vector<const constraint_type*> ics;

          auto test = [&ics] (build_package* pv, build_package* pc)
            -> const constraint_type*
          {
            for (const constraint_type& c: pc->constraints)
            {
              if (find (ics.begin (), ics.end (), &c) == ics.end () &&
                  !satisfies (pv->available_version (), c.value))
                return &c;
            }

            return nullptr;
          };

          // Iterate until one of the versions becomes satisfactory due to
          // ignoring some of the constraints.
          //
          for (;;)
          {
            // First see if p1 satisfies p2's constraints.
            //
            if (auto c2 = test (p1, p2))
            {
              // If not, try the other way around.
              //
              if (auto c1 = test (p2, p1))
              {
                // Add a constraint to the igrore-list and the dependent to
                // the unsatisfied-list.
                //
                const constraint_type* c (p1 == &bp ? c2 : c1);
                const build_package* p (dependent_build (*c));

                // Note that if one of the constraints is imposed on the
                // package by the command line, then another constraint must
                // be imposed by a dependent. Also, in this case it feels that
                // the map must contain the dependency constrained by the
                // command line and so p may not be NULL. If that (suddenly)
                // is not the case, then we will have to ignore the constraint
                // imposed by the dependent which is not in the map, replace
                // the version, and call replace_ver().
                //
                if (p == nullptr)
                {
                  c = (c == c1 ? c2 : c1);
                  p = dependent_build (*c);

                  // One of the dependents must be a real package.
                  //
                  assert (p != nullptr);
                }

                ics.push_back (c);

                package_key d (p->db, p->name ());

                l5 ([&]{trace << "postpone failure for dependent " << d
                              << " unsatisfied with dependency "
                              << bp.available_name_version_db () << " ("
                              << c->value << ')';});

                // Note that in contrast to collect_dependents(), here we also
                // save both unsatisfied constraints and the dependency chain,
                // for the sake of the diagnostics.
                //
                vector<unsatisfied_constraint> ucs {
                  unsatisfied_constraint {
                    *c1, p1->available_version (), p1->system},
                  unsatisfied_constraint {
                    *c2, p2->available_version (), p2->system}};

                vector<package_key> dc;

                if (dep_chain != nullptr)
                {
                  dc.reserve (dep_chain->size ());

                  for (const build_package& p: *dep_chain)
                    dc.emplace_back (p.db, p.name ());
                }

                unsatisfied_depts.add (d, pk, c->value, move (ucs), move (dc));
                continue;
              }
              else
                swap (p1, p2);
            }

            break;
          }

          l4 ([&]{trace << "pick " << p1->available_name_version_db ()
                        << " over " << p2->available_name_version_db ();});
        }

        // See if we are replacing the object. If not, then we don't need to
        // collect its prerequisites since that should have already been done.
        // Remember, p1 points to the object we want to keep.
        //
        bool replace (p1 != &bp);

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
            // removing it from the dependency build_package. Maybe/later.
            //
            // NOTE: remember to update collect_drop() if changing anything
            //       here.
            //
            bool scratch (true);

            // While checking if the package has any dependencies skip the
            // toolchain build-time dependencies since they should be quite
            // common.
            //
            // An update: it turned out that just the absence of dependencies
            // is not the only condition that causes a package to be replaced
            // in place. The following conditions must also be met:
            //
            // - The package must not participate in any configuration
            //   negotiation on the dependency side (otherwise we could have
            //   missed collecting its existing dependents).
            //
            // - The package up/downgrade doesn't cause the selection of a
            //   different dependency alternative for any of its dependents
            //   (see postponed_packages for possible outcomes).
            //
            // - The package must not be added to unsatisfied_depts on the
            //   dependency side.
            //
            // This all sounds quite hairy at the moment, so we won't be
            // replacing in place for now (which is an optimization).
#if 0
            if (!has_dependencies (options, p2->available->dependencies))
              scratch = false;
#endif
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
                                   *dep_chain,
                                   fdb,
                                   apc,
                                   *rpt_depts,
                                   replaced_vers,
                                   postponed_repo,
                                   postponed_alts,
                                   0 /* max_alt_index */,
                                   *postponed_recs,
                                   *postponed_edeps,
                                   *postponed_deps,
                                   postponed_cfgs,
                                   *unacceptable_alts,
                                   unsatisfied_depts);

    return &p;
  }

  optional<build_packages::pre_reevaluate_result> build_packages::
  collect_build_prerequisites (const pkg_build_options& options,
                               build_package& pkg,
                               build_package_refs& dep_chain,
                               const function<find_database_function>& fdb,
                               const function<add_priv_cfg_function>& apc,
                               const repointed_dependents& rpt_depts,
                               replaced_versions& replaced_vers,
                               postponed_packages* postponed_repo,
                               postponed_packages* postponed_alts,
                               size_t max_alt_index,
                               postponed_packages& postponed_recs,
                               postponed_existing_dependencies& postponed_edeps,
                               postponed_dependencies& postponed_deps,
                               postponed_configurations& postponed_cfgs,
                               unacceptable_alternatives& unacceptable_alts,
                               unsatisfied_dependents& unsatisfied_depts,
                               optional<pair<size_t, size_t>> reeval_pos,
                               const optional<package_key>& orig_dep)
  {
    // NOTE: don't forget to update collect_build_postponed() if changing
    //       anything in this function. Also enable and run the tests with the
    //       config.bpkg.tests.all=true variable when done.
    //
    tracer trace ("collect_build_prerequisites");

    assert (pkg.action && *pkg.action == build_package::build);

    const package_name& nm (pkg.name ());
    database& pdb (pkg.db);
    package_key pk (pdb, nm);

    bool pre_reeval (reeval_pos && reeval_pos->first == 0);
    assert (!pre_reeval || reeval_pos->second == 0);

    // Must only be specified in the pre-reevaluation mode.
    //
    assert (orig_dep.has_value () == pre_reeval);

    bool reeval (reeval_pos && reeval_pos->first != 0);
    assert (!reeval || reeval_pos->second != 0);

    // The being (pre-)re-evaluated dependent cannot be recursively collected
    // yet. Also, we don't expect it being configured as system.
    //
    // Note that the configured package can still be re-evaluated after
    // collect_build_prerequisites() has been called but didn't end up with
    // the recursive collection.
    //
    assert ((!pre_reeval && !reeval)   ||
            ((!pkg.recursive_collection ||
              !pkg.recollect_recursively (rpt_depts)) &&
             !pkg.skeleton && !pkg.system));

    // If this package is not being (pre-)re-evaluated, is not yet collected
    // recursively, needs to be reconfigured, and is not yet postponed, then
    // check if it is a dependency of any dependent with configuration clause
    // and postpone the collection if that's the case.
    //
    // The reason why we don't need to do this for the re-evaluated case is as
    // follows: this logic is used for an existing dependent that is not
    // otherwise built (e.g., reconfigured) which means its externally-
    // imposed configuration (user, dependents) is not being changed.
    //
    if (!pre_reeval                                    &&
        !reeval                                        &&
        !pkg.recursive_collection                      &&
        pkg.reconfigure ()                             &&
        postponed_cfgs.find_dependency (pk) == nullptr &&
        postponed_edeps.find (pk) == postponed_edeps.end ())
    {
      // Note that there can be multiple existing dependents for a dependency.
      // Also note that we skip the existing dependents for which re-
      // evaluation is optional not to initiate any negotiation in a simple
      // case (see collect_build_prerequisites() description for details).
      //
      vector<existing_dependent> eds (
        query_existing_dependents (trace,
                                   options,
                                   pk.db,
                                   pk.name,
                                   true /* exclude_optional */,
                                   fdb,
                                   rpt_depts,
                                   replaced_vers));

      if (!eds.empty ())
      {
        bool postpone (false);

        for (existing_dependent& ed: eds)
        {
          if (ed.dependency) // Configuration clause is encountered.
          {
            const build_package* bp (&pkg);

            package_key& dep (*ed.dependency);
            package_key  dpt (ed.db, ed.selected->name);

            // If the earliest configuration clause applies to a different
            // dependency, then collect it (non-recursively).
            //
            if (dep != pk)
              bp = collect_existing_dependent_dependency (options,
                                                          ed,
                                                          replaced_vers,
                                                          postponed_cfgs,
                                                          unsatisfied_depts);

            // If the dependency collection has already been postponed, then
            // indicate that the dependent with configuration clauses is also
            // present and thus the postponement is not bogus. But only add
            // the new entry to postponed_deps and throw the
            // postpone_dependency exception if the dependency is already
            // collected. Note that adding the new entry unconditionally would
            // be a bad idea, since by postponing the dependency collection we
            // may not see its existing dependent with a configuration
            // clauses, end up with a bogus postponement, and start
            // yo-yoing. In other words, we add the entry only if absolutely
            // necessary (who knows, maybe the existing dependent will be
            // dropped before we try to collect it recursively).
            //
            auto i (postponed_deps.find (dep));

            if (i != postponed_deps.end ())
              i->second.with_config = true;

            // Prematurely collected before we saw any config clauses.
            //
            if (bp->recursive_collection)
            {
              l5 ([&]{trace << "cannot cfg-postpone dependency "
                            << bp->available_name_version_db ()
                            << " of existing dependent " << dpt
                            << " (collected prematurely), "
                            << "throwing postpone_dependency";});

              if (i == postponed_deps.end ())
              {
                postponed_deps.emplace (dep,
                                        postponed_dependency {
                                          false /* without_config */,
                                          true  /* with_config */});
              }

              // Don't print the "while satisfying..." chain.
              //
              dep_chain.clear ();

              throw postpone_dependency (move (dep));
            }

            l5 ([&]{trace << "cfg-postpone dependency "
                          << bp->available_name_version_db ()
                          << " of existing dependent " << *ed.selected
                          << ed.db << " due to dependency "
                          << pkg.available_name_version_db ();});

            collect_existing_dependent (options,
                                        ed,
                                        {pk},
                                        replaced_vers,
                                        postponed_cfgs,
                                        unsatisfied_depts);

            // Only add this dependent/dependency to the newly created cluster
            // if this dependency doesn't belong to any cluster yet, which may
            // not be the case if there are multiple existing dependents with
            // configuration clause for this dependency.
            //
            // To put it another way, if there are multiple such existing
            // dependents for this dependency, here we will create the
            // configuration cluster only for the first one. The remaining
            // dependents will be added to this dependency's cluster when the
            // existing dependents of dependencies in this cluster are all
            // discovered and reevaluated (see collect_build_postponed() for
            // details).
            //
            if (postponed_cfgs.find_dependency (dep) == nullptr)
              postponed_cfgs.add (move (dpt),
                                  ed.dependency_position,
                                  move (dep));
          }
          else // Existing dependent is deviated.
          {
            // Note that we could probably re-collect deviated dependents
            // recursively right away but such a two-directional recursion
            // would complicate implementation and troubleshooting. Thus,
            // given that the deviated dependents are not very common, we just
            // postpone their re-collection.
            //
            l5 ([&]{trace << "schedule re-collection of deviated "
                          << "existing dependent " << *ed.selected
                          << ed.db;});

            recollect_existing_dependent (options,
                                          ed,
                                          replaced_vers,
                                          postponed_recs,
                                          postponed_cfgs,
                                          unsatisfied_depts,
                                          true /* add_required_by */);
          }

          // Postpone the recursive collection of a dependency if the existing
          // dependent has deviated or the dependency belongs to the earliest
          // depends clause with configuration clause or to some later depends
          // clause. It is supposed that it will be collected during its
          // existing dependent re-collection.
          //
          if (!ed.dependency || // Dependent has deviated.
              ed.originating_dependency_position >= ed.dependency_position)
          {
            postpone = true;
            postponed_edeps[pk].emplace_back (ed.db, ed.selected->name);
          }
        }

        if (postpone)
          return nullopt;
      }
    }

    pkg.recursive_collection = true;

    if (pkg.system)
    {
      l5 ([&]{trace << "skip system " << pkg.available_name_version_db ();});
      return nullopt;
    }

    const shared_ptr<available_package>& ap (pkg.available);
    assert (ap != nullptr);

    const shared_ptr<selected_package>& sp (pkg.selected);

    assert ((!pre_reeval && !reeval) || sp != nullptr);

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
    bool src_conf (sp != nullptr                          &&
                   sp->state == package_state::configured &&
                   sp->substate != package_substate::system);

    // The being (pre-)re-evaluated dependent must be configured as a source
    // package and should not be collected recursively (due to upgrade, etc).
    //
    assert ((!pre_reeval && !reeval) ||
            (src_conf && !pkg.recollect_recursively (rpt_depts)));

    if (src_conf)
    {
      if (!pre_reeval && !reeval && !pkg.recollect_recursively (rpt_depts))
      {
        l5 ([&]{trace << "skip configured "
                      << pkg.available_name_version_db ();});
        return nullopt;
      }

      repointed_dependents::const_iterator i (rpt_depts.find (pk));

      if (i != rpt_depts.end ())
        rpt_prereq_flags = &i->second;
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
      l5 ([&]{trace << (pre_reeval ? "pre-reeval " :
                        reeval     ? "reeval "     :
                                     "begin "      )
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
      return nullopt;
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

    if (!pre_reeval)
      dep_chain.push_back (pkg);

    assert (sdeps.size () < deps.size ());

    package_skeleton& skel (*pkg.skeleton);

    // We shouldn't be failing in the reevaluation mode, given that we only
    // reevaluate a package if its pre-reevaluation succeeds.
    //
    auto fail_reeval = [&pkg] ()
    {
      fail << "unable to re-create dependency information of already "
           << "configured package " << pkg.available_name_version_db () <<
        info << "likely cause is change in external environment" <<
        info << "if not, please report in https://github.com/build2/build2/issues/302";
    };

    bool postponed (false);
    bool reevaluated (false);

    // In the pre-reevaluation mode keep track of configuration variable
    // prefixes similar to what we do in pkg_configure_prerequisites(). Stop
    // tracking if we discovered that the dependent re-evaluation is not
    // optional.
    //
    vector<string> banned_var_prefixes;

    auto references_banned_var = [&banned_var_prefixes] (const string& clause)
    {
      for (const string& p: banned_var_prefixes)
      {
        if (clause.find (p) != string::npos)
          return true;
      }

      return false;
    };

    if (pre_reeval)
    {
      if (!sp->dependency_alternatives_section.loaded ())
        pdb.load (*sp, sp->dependency_alternatives_section);

      // It doesn't feel like the number of depends clauses may differ for the
      // available and selected packages in the pre-reevaluation mode since
      // they must refer to the same package version. If it still happens,
      // maybe due to some manual tampering, let's assume this as a deviation
      // case.
      //
      size_t nn (deps.size ());
      size_t on (sp->dependency_alternatives.size ());

      if (nn != on)
      {
        l5 ([&]{trace << "re-evaluation of dependent "
                      << pkg.available_name_version_db ()
                      << " deviated: number of depends clauses changed to "
                      << nn << " from " << on;});

        throw reevaluation_deviated ();
      }
    }

    pre_reevaluate_result r;

    for (size_t di (sdeps.size ()); di != deps.size (); ++di)
    {
      // Fail if we missed the re-evaluation target position for any reason.
      //
      if (reeval && di == reeval_pos->first) // Note: reeval_pos is 1-based.
        fail_reeval ();

      const dependency_alternatives_ex& das (deps[di]);

      // Add an empty alternatives list into the selected dependency list if
      // this is a toolchain build-time dependency.
      //
      dependency_alternatives_ex sdas (das.buildtime, das.comment);

      if (toolchain_buildtime_dependency (options, das, &nm))
      {
        if (pre_reeval)
        {
          size_t oi (sp->dependency_alternatives[di]);

          // It doesn't feel like it may happen in the pre-reevaluation
          // mode. If it still happens, maybe due to some manual tampering,
          // let's assume this as a deviation case.
          //
          if (oi != 0)
          {
            l5 ([&]{trace << "re-evaluation of dependent "
                          << pkg.available_name_version_db ()
                          << " deviated at depends clause " << di + 1
                          << ": toolchain buildtime dependency replaced the "
                          << " regular one with selected alternative " << oi;});

            throw reevaluation_deviated ();
          }
        }

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

          bool enabled;

          if (da.enable)
          {
            if (pre_reeval              &&
                r.reevaluation_optional &&
                references_banned_var (*da.enable))
            {
              r.reevaluation_optional = false;
            }

            enabled = skel.evaluate_enable (*da.enable, make_pair (di, i));
          }
          else
            enabled = true;

          if (enabled)
            edas.push_back (make_pair (ref (da), i));
        }
      }

      if (edas.empty ())
      {
        if (pre_reeval)
        {
          size_t oi (sp->dependency_alternatives[di]);

          if (oi != 0)
          {
            l5 ([&]{trace << "re-evaluation of dependent "
                          << pkg.available_name_version_db ()
                          << " deviated at depends clause " << di + 1
                          << ": dependency with previously selected "
                          << "alternative " << oi << " is now disabled";});

            throw reevaluation_deviated ();
          }
        }

        sdeps.push_back (move (sdas));
        salts.push_back (0);           // Keep parallel to sdeps.
        continue;
      }

      // Try to pre-collect build information (pre-builds) for the
      // dependencies of an alternative. Optionally, issue diagnostics into
      // the specified diag record. In the dry-run mode don't change the
      // packages collection state (postponed_repo set, etc).
      //
      // If an alternative dependency package is specified as a dependency
      // with a version constraint on the command line, then overwrite the
      // dependent's constraint with the command line's constraint, if the
      // latter is a subset of former. If it is not a subset, then bail out
      // indicating that the alternative dependencies cannot be resolved
      // (builds is nullopt), unless ignore_unsatisfactory_dep_spec argument
      // is true. In the latter case continue precollecting as if no
      // constraint is specified on the command line for this dependency. That
      // will likely result in the unsatisfied dependent problem, which will
      // be either resolved or end up with the failure (see
      // unsatisfied_dependents for details).
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

        // If true is passed as the check_constraints argument to precollect()
        // and some dependency of the alternative cannot be resolved because
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
                         pre_reeval,
                         &trace,
                         this]
        (const dependency_alternative& da,
         bool buildtime,
         const package_prerequisites* prereqs,
         bool check_constraints,
         bool ignore_unsatisfactory_dep_spec,
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
              // It doesn't feel like it may happen in the pre-reevaluation
              // mode. If it still happens, maybe due to some manual
              // tampering, let's assume this as a deviation case.
              //
              if (pre_reeval)
              {
                l5 ([&]{trace << "re-evaluation of dependent "
                              << pkg.available_name_version_db ()
                              << " deviated: build-time dependency " << dn
                              << " is now in build system module "
                              << "configuration";});

                throw reevaluation_deviated ();
              }

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
                  !bp.constraints.empty ())
              {
                assert (bp.constraints.size () == 1);

                const build_package::constraint_type& c (bp.constraints[0]);

                // If the user-specified dependency constraint is the wildcard
                // version, then it satisfies any dependency constraint.
                //
                if (!wildcard (c.value) && !satisfies (c.value, dp.constraint))
                {
                  // We should end up throwing reevaluation_deviated exception
                  // before the diagnostics run in the pre-reevaluation mode.
                  //
                  assert (!pre_reeval || dr == nullptr);

                  if (!ignore_unsatisfactory_dep_spec)
                  {
                    if (dr != nullptr)
                    {
                      //             "  info: ..."
                      string indent ("          ");

                      *dr << error << "unable to satisfy constraints on package "
                          << dn <<
                        info << nm << pdb << " depends on (" << dn << ' '
                          << *dp.constraint << ')';

                      {
                        set<package_key> printed;
                        print_constraints (*dr, pkg, indent, printed);
                      }

                      *dr << info << c.dependent << " depends on (" << dn << ' '
                          << c.value << ')';

                      if (const build_package* d = dependent_build (c))
                      {
                        set<package_key> printed;
                        print_constraints (*dr, *d, indent, printed);
                      }

                      *dr << info << "specify " << dn << " version to satisfy "
                          << nm << " constraint";
                    }

                    return precollect_result (false /* postpone */);
                  }
                }
                else
                {
                  dep_constr = &c.value;
                  system = bp.system;
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
                // If it happens in the pre-reevaluation mode, that may mean
                // that the package has become broken since the time the
                // dependent was built. Let's assume this as a deviation case
                // and fail on the re-collection.
                //
                if (pre_reeval)
                {
                  l5 ([&]{trace << "re-evaluation of dependent "
                                << pkg.available_name_version_db ()
                                << " deviated: package " << dn << *ddb
                                << " is broken";});

                  throw reevaluation_deviated ();
                }

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
                    // If it happens in the pre-reevaluation mode, that may
                    // mean that some new configuration has been linked since
                    // the time the dependent was built. Let's assume this as
                    // a deviation case.
                    //
                    if (pre_reeval)
                    {
                      l5 ([&]{trace << "re-evaluation of dependent "
                                    << pkg.available_name_version_db ()
                                    << " deviated: now multiple possible "
                                    << type << " configurations for "
                                    << "build-time dependency (" << dp << "): "
                                    << db->config_orig << ", "
                                    << ldb.config_orig;});

                      throw reevaluation_deviated ();
                    }

                    assert (dr == nullptr); // Should fail on the "silent" run.

                    fail << "multiple possible " << type << " configurations "
                         << "for build-time dependency (" << dp << ')' <<
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
                // If it happens in the pre-reevaluation mode, that may mean
                // that some configuration has been unlinked since the time
                // the dependent was built. Let's assume this as a deviation
                // case.
                //
                if (pre_reeval)
                {
                  l5 ([&]{trace << "re-evaluation of dependent "
                                << pkg.available_name_version_db ()
                                << " deviated: now no suitable configuration "
                                << "is found for build-time dependency ("
                                << dp << ')';});

                  throw reevaluation_deviated ();
                }

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

                // Use the *-no-warnings host/build2 configurations since the
                // user has no control over such private configurations and
                // they are primarily used for consumption.
                //
                const strings vars {
                  "config.config.load=~" + type + "-no-warnings",
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
              // It doesn't feel like it may happen in the pre-reevaluation
              // mode. If it still happens, maybe due to some manual
              // tampering, let's assume this as a deviation case.
              //
              if (pre_reeval)
              {
                l5 ([&]{trace << "re-evaluation of dependent "
                              << pkg.available_name_version_db ()
                              << " deviated: now unable to build build system "
                              << "module " << dn << " in its dependent "
                              << "package configuration " << pdb.config_orig;});

                throw reevaluation_deviated ();
              }

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
                // If it happens in the pre-reevaluation mode, that may mean
                // that the dependent has become an orphan since the time it
                // was built. Let's assume this as a deviation case.
                //
                if (pre_reeval)
                {
                  l5 ([&]{trace << "re-evaluation of dependent "
                                << pkg.available_name_version_db ()
                                << " deviated: is now orphaned";});

                  throw reevaluation_deviated ();
                }

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
              rp = find_existing (dn, d.constraint, af);

              if (dap == nullptr)
                rp = find_available_one (dn, d.constraint, af);

              if (dap == nullptr && system && d.constraint)
                rp = find_available_one (dn, nullopt, af);

              if (dap == nullptr)
              {
                // If it happens in the pre-reevaluation mode, that may mean
                // that the repositories has been refetched since the time the
                // dependent was built and don't contain a satisfactory
                // package anymore. Let's assume this as a deviation case.
                //
                if (pre_reeval)
                {
                  l5 ([&]{trace << "re-evaluation of dependent "
                                << pkg.available_name_version_db ()
                                << " deviated: unable to satisfy "
                                << (!dep_constr ? "" : "user-specified ")
                                << "dependency constraint (" << d << ')';});

                  throw reevaluation_deviated ();
                }

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
                    info << "or the repository metadata could be stale" <<
                    info << "run 'bpkg rep-fetch' (or equivalent) to update";
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
                  // We should end up throwing reevaluation_deviated exception
                  // before the diagnostics run in the pre-reevaluation mode.
                  //
                  assert (!pre_reeval || dr == nullptr);

                  if (dr != nullptr)
                    *dr << error << "dependency " << d << " of package "
                        << nm << " is not available in source" <<
                      info << "specify ?sys:" << dn << " if it is available "
                        << "from the system";

                  return precollect_result (false /* postpone */);
                }

                if (!satisfies (*dap->system_version (*ddb), d.constraint))
                {
                  // We should end up throwing reevaluation_deviated exception
                  // before the diagnostics run in the pre-reevaluation mode.
                  //
                  assert (!pre_reeval || dr == nullptr);

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

          // Now, as we have pre-collected the dependency builds, if
          // requested, go through them and check that for those dependencies
          // which are already being built we will be able to choose one of
          // them (either existing or new) which satisfies all the dependents.
          // If that's not the case, then issue the diagnostics, if requested,
          // and return the unsatisfactory dependency builds.
          //
          // Note that collect_build() also performs this check but postponing
          // it till then can end up in failing instead of selecting some
          // other dependency alternative.
          //
          if (check_constraints)
          {
            for (const prebuild& b: r)
            {
              const shared_ptr<available_package>& dap (b.available);

              // Otherwise we would have failed earlier.
              //
              assert (dap != nullptr);

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

                    constraint_type c1 (*d.constraint,
                                        pdb,
                                        nm,
                                        pkg.available_version (),
                                        false /* existing_dependent */);

                    if (!satisfies (v2, c1.value))
                    {
                      for (const constraint_type& c2: bp.constraints)
                      {
                        if (!satisfies (v1, c2.value))
                        {
                          // We should end up throwing reevaluation_deviated
                          // exception before the diagnostics run in the
                          // pre-reevaluation mode.
                          //
                          assert (!pre_reeval || dr == nullptr);

                          if (dr != nullptr)
                          {
                            const package_name& n (d.name);

                            //             "  info: ..."
                            string indent ("          ");

                            *dr << error << "unable to satisfy constraints on "
                                << "package " << n <<
                              info << c2.dependent << " depends on (" << n
                                   << ' ' << c2.value << ')';

                            if (const build_package* d = dependent_build (c2))
                            {
                              set<package_key> printed;
                              print_constraints (*dr, *d, indent, printed);
                            }

                            *dr << info << c1.dependent << " depends on ("
                                        << n << ' ' << c1.value << ')';

                            if (const build_package* d = dependent_build (c1))
                            {
                              set<package_key> printed;
                              print_constraints (*dr, *d, indent, printed);
                            }

                            *dr << info << "available "
                                << bp.available_name_version () <<
                              info << "available "
                                   << package_string (n, v1, b.system) <<
                              info << "explicitly specify " << n
                                   << " version to manually satisfy both "
                                   << "constraints";
                          }

                          return precollect_result (reused, move (r));
                        }
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
                      &replaced_vers,
                      &dep_chain,
                      postponed_repo,
                      postponed_alts,
                      &postponed_recs,
                      &postponed_edeps,
                      &postponed_deps,
                      &postponed_cfgs,
                      &unacceptable_alts,
                      &unsatisfied_depts,
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
         const package_prerequisites* prereqs,
         bool check_constraints,
         bool ignore_unsatisfactory_dep_spec)
        {
          // Dependency alternative position.
          //
          pair<size_t, size_t> dp (di + 1, dai + 1);

          if (reeval                         &&
              dp.first  == reeval_pos->first &&
              dp.second != reeval_pos->second)
            fail_reeval ();

          postponed_configuration::packages cfg_deps;

          // Remove the temporary dependency collection postponements (see
          // below for details).
          //
          postponed_configuration::packages temp_postponements;

          auto g (
            make_guard (
              [&temp_postponements, &postponed_deps] ()
              {
                for (const package_key& d: temp_postponements)
                  postponed_deps.erase (d);
              }));

          package_version_key pvk (pk.db, pk.name, pkg.available_version ());

          for (prebuild& b: bs)
          {
            build_package bpk {
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
              nullopt,                    // Upgrade.
              false,                      // Deorphan.
              {pvk},                      // Required by (dependent).
              true,                       // Required by dependents.
              0};                         // State flags.

            const optional<version_constraint>& constraint (
              b.dependency.constraint);

            // Add our constraint, if we have one.
            //
            // Note that we always add the constraint implied by the
            // dependent. The user-implied constraint, if present, will be
            // added when merging from the pre-entered entry. So we will have
            // both constraints for completeness.
            //
            if (constraint)
              bpk.constraints.emplace_back (*constraint,
                                            pdb,
                                            nm,
                                            pkg.available_version (),
                                            false /* existing_dependent */);

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
                             move (bpk),
                             replaced_vers,
                             postponed_cfgs,
                             unsatisfied_depts,
                             &dep_chain,
                             nullptr /* fdb */,
                             nullptr /* apc */,
                             nullptr /* rpt_depts */,
                             nullptr /* postponed_repo */,
                             nullptr /* postponed_alts */,
                             nullptr /* postponed_recs */,
                             nullptr /* postponed_edeps */,
                             nullptr /* postponed_deps */,
                             nullptr /* unacceptable_alts */,
                             verify));

            package_key dpk (b.db, b.available->id.name);

            // Do not collect prerequisites recursively for dependent
            // re-evaluation. Instead, if the re-evaluation position is
            // reached, stash the dependency packages to add them to the
            // existing dependent's cluster.
            //
            if (reeval && dp != *reeval_pos)
              continue;

            // Note that while collect_build() may prefer an existing entry in
            // the map and return NULL, the recursive collection of this
            // preferred entry may has been postponed due to the existing
            // dependent (see collect_build_prerequisites() for details). Now,
            // we can potentially be recursively collecting such a dependent
            // after its re-evaluation to some earlier than this dependency
            // position. If that's the case, it is the time to collect this
            // dependency (unless it has a config clause which will be handled
            // below).
            //
            if (p == nullptr)
            {
              p = entered_build (dpk);

              // We don't expect the collected build to be replaced with the
              // drop since its required-by package names have the "required
              // by dependents" semantics.
              //
              assert (p != nullptr &&
                      p->action    &&
                      *p->action == build_package::build);
            }

            bool collect_prereqs (!p->recursive_collection);

            // Do not recursively collect a dependency of a dependent with
            // configuration clauses, which could be this or some other
            // (indicated by the presence in postponed_deps or postponed_cfgs)
            // dependent. In the former case if the prerequisites were
            // prematurely collected, throw postpone_dependency.
            //
            // Note that such a dependency will be recursively collected
            // directly right after the configuration negotiation (rather than
            // via the dependent).
            //
            {
              if (da.prefer || da.require)
              {
                // If the dependency collection has already been postponed,
                // then indicate that the dependent with configuration clauses
                // is also present and thus the postponement is not bogus.
                // Otherwise, if the dependency is already recursively
                // collected (and we are not up-negotiating; see below) then
                // add the new entry to postponed_deps and throw the
                // postpone_dependency exception. Otherwise, temporarily add
                // the new entry for the duration of the dependency collection
                // loop to prevent recursive collection of this dependency via
                // some other dependent. When out of the loop, we will add the
                // dependency into some configuration cluster, effectively
                // moving the dependency postponement information from
                // postponed_deps to postponed_cfgs. Note that generally we
                // add new entries to postponed_deps only if absolutely
                // necessary to avoid yo-yoing (see the initial part of the
                // collect_build_prerequisites() function for details).
                //
                auto i (postponed_deps.find (dpk));

                bool new_postponement (false);

                if (i == postponed_deps.end ())
                {
                  postponed_deps.emplace (dpk,
                                          postponed_dependency {
                                            false /* without_config */,
                                            true  /* with_config */});
                  new_postponement = true;
                }
                else
                  i->second.with_config = true;

                // Throw postpone_dependency if the dependency is prematurely
                // collected before we saw any config clauses.
                //
                // Note that we don't throw if the dependency already belongs
                // to some (being) negotiated cluster since we will
                // up-negotiate its configuration (at the end of the loop)
                // instead.
                //
                if (p->recursive_collection)
                {
                  const postponed_configuration* pcfg (
                    postponed_cfgs.find_dependency (dpk));

                  // Can it happen that an already recursively collected
                  // dependency (recursive_collection is true) belongs to a
                  // non (being) negotiated cluster? Yes, if, in particular,
                  // this dependency is an already re-evaluated existing
                  // dependent and we are currently re-evaluating its own
                  // existing dependent and its (as a dependency) cluster is
                  // not being negotiated yet (is in the existing dependents
                  // re-evaluation phase). See the
                  // pkg-build/.../collected-dependency-non-negotiated-cluster
                  // test for an example.
                  //
                  if (!(pcfg != nullptr && pcfg->negotiated))
                  {
                    if (reeval)
                    {
                      l5 ([&]{trace << "cannot re-evaluate existing dependent "
                                    << pkg.available_name_version_db ()
                                    << " due to dependency "
                                    << p->available_name_version_db ()
                                    << " (collected prematurely), "
                                    << "throwing postpone_dependency";});
                    }
                    else
                    {
                      l5 ([&]{trace << "cannot cfg-postpone dependency "
                                    << p->available_name_version_db ()
                                    << " of dependent "
                                    << pkg.available_name_version_db ()
                                    << " (collected prematurely), "
                                    << "throwing postpone_dependency";});
                    }

                    // Don't print the "while satisfying..." chain.
                    //
                    dep_chain.clear ();

                    throw postpone_dependency (move (dpk));
                  }
                }

                if (new_postponement)
                  temp_postponements.push_back (dpk);

                if (!reeval)
                {
                  // Postpone until (re-)negotiation.
                  //
                  l5 ([&]{trace << "cfg-postpone dependency "
                                << p->available_name_version_db ()
                                << " of dependent "
                                << pkg.available_name_version_db ();});
                }

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
                                << p->available_name_version_db ()
                                << " of dependent "
                                << pkg.available_name_version_db ();});

                  i->second.wout_config = true;

                  collect_prereqs = false;
                }
                else
                {
                  const postponed_configuration* pcfg (
                    postponed_cfgs.find_dependency (dpk));

                  if (pcfg != nullptr)
                  {
                    l5 ([&]{trace << "dep-postpone dependency "
                                  << p->available_name_version_db ()
                                  << " of dependent "
                                  << pkg.available_name_version_db ()
                                  << " since already in cluster " << *pcfg;});

                    collect_prereqs = false;
                  }
                  else
                  {
                    l5 ([&]{trace << "no cfg-clause for dependency "
                                  << p->available_name_version_db ()
                                  << " of dependent "
                                  << pkg.available_name_version_db ();});
                  }
                }
              }
            }

            if (collect_prereqs)
              collect_build_prerequisites (options,
                                           *p,
                                           dep_chain,
                                           fdb,
                                           apc,
                                           rpt_depts,
                                           replaced_vers,
                                           postponed_repo,
                                           postponed_alts,
                                           0 /* max_alt_index */,
                                           postponed_recs,
                                           postponed_edeps,
                                           postponed_deps,
                                           postponed_cfgs,
                                           unacceptable_alts,
                                           unsatisfied_depts);
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
              bool unsatisfactory (false);

              for (++i; i != edas.size (); ++i)
              {
                if (unacceptable ())
                  continue;

                const dependency_alternative& a (edas[i].first);

                precollect_result r (
                  precollect (a,
                              das.buildtime,
                              prereqs,
                              check_constraints,
                              ignore_unsatisfactory_dep_spec,
                              nullptr /* diag_record */,
                              true /* dry_run */));

                if (r.builds && r.reused)
                {
                  has_alt = true;
                  break;
                }

                if (r.unsatisfactory)
                  unsatisfactory = true;
              }

              // If there are none and we are in the "recreate dependency
              // decisions" mode, then repeat the search in the "make
              // dependency decisions" mode.
              //
              if (!has_alt && prereqs != nullptr)
              {
                unsatisfactory = false;

                for (i = 0; i != edas.size (); ++i)
                {
                  if (unacceptable ())
                    continue;

                  const dependency_alternative& a (edas[i].first);

                  if (&a != &da) // Skip the current dependency alternative.
                  {
                    precollect_result r (
                      precollect (a,
                                  das.buildtime,
                                  nullptr /* prereqs */,
                                  check_constraints,
                                  ignore_unsatisfactory_dep_spec,
                                  nullptr /* diag_record */,
                                  true /* dry_run */));

                    if (r.builds && r.reused)
                    {
                      has_alt = true;
                      break;
                    }

                    if (r.unsatisfactory)
                      unsatisfactory = true;
                  }
                }
              }

              // If there are none and we are in the "check constraints" mode,
              // then repeat the search with this mode off.
              //
              bool cc (check_constraints);
              if (!has_alt && check_constraints && unsatisfactory)
              {
                cc = false;

                for (i = 0; i != edas.size (); ++i)
                {
                  if (unacceptable ())
                    continue;

                  const dependency_alternative& a (edas[i].first);

                  if (&a != &da) // Skip the current dependency alternative.
                  {
                    precollect_result r (
                      precollect (a,
                                  das.buildtime,
                                  nullptr /* prereqs */,
                                  false /* check_constraints */,
                                  ignore_unsatisfactory_dep_spec,
                                  nullptr /* diag_record */,
                                  true /* dry_run */));

                    if (r.builds && r.reused)
                    {
                      has_alt = true;
                      break;
                    }
                  }
                }
              }

              if (!has_alt && !ignore_unsatisfactory_dep_spec)
              {
                for (i = 0; i != edas.size (); ++i)
                {
                  if (unacceptable ())
                    continue;

                  const dependency_alternative& a (edas[i].first);

                  if (&a != &da) // Skip the current dependency alternative.
                  {
                    precollect_result r (
                      precollect (a,
                                  das.buildtime,
                                  nullptr /* prereqs */,
                                  cc,
                                  true /* ignore_unsatisfactory_dep_spec */,
                                  nullptr /* diag_record */,
                                  true /* dry_run */));

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

              // As a first step add this dependent/dependencies to one of the
              // new/existing postponed_configuration clusters, which could
              // potentially cause some of them to be merged. Note that when
              // we re-evaluate existing dependents of dependencies in a
              // cluster, these dependents can potentially be added to
              // different clusters (see collect_build_postponed() for
              // details). Here are the possibilities and what we should do in
              // each case.
              //
              // 1. Got added to a new cluster -- this dependent got postponed
              //    and we return false.
              //
              // 2. Got added to an existing non-yet-negotiated cluster (which
              //    could potentially involve merging a bunch of them) --
              //    ditto. Note this also covers adding into a cluster which
              //    contain dependencies whose existing dependents we are
              //    currently re-evaluating (the negotiated member is absent
              //    but the depth is non-zero).
              //
              // 3. Got added to an existing already-negotiated cluster (which
              //    could potentially involve merging a bunch of them, some
              //    negotiated and some not yet negotiated). Perhaps just
              //    making the resulting cluster shadow and rolling back, just
              //    like in the other case (non-existing dependent), will do.
              //
              postponed_configuration& cfg (
                postponed_cfgs.add (pk,
                                    true /* existing */,
                                    dp,
                                    cfg_deps,
                                    has_alt).first);

              if (cfg.negotiated) // Case (3).
              {
                // Note that the closest cluster up on the stack is in the
                // existing dependents re-evaluation phase and thus is not
                // being negotiated yet. The following clusters up on the
                // stack can only be in the (fully) negotiated state. Thus, if
                // cfg.negotiated member is present it can only be true.
                //
                // Also as a side-note: at any given moment there can only be
                // 0 or 1 cluster being negotiated (the negotiate member is
                // false).
                //
                assert (*cfg.negotiated);

                // Don't print the "while satisfying..." chain.
                //
                dep_chain.clear ();

                // There is just one complication:
                //
                // If the shadow cluster is already present and it is exactly
                // the same as the resulting cluster which we are going to
                // make a shadow, then we have already been here and we may
                // start yo-yoing. To prevent that we will throw the
                // merge_configuration_cycle exception instead of
                // merge_configuration, so that the caller could handle this
                // situation, for example, by just re-collecting the being
                // re-evaluated existing dependent from scratch, reducing this
                // case to the regular up-negotiating.
                //
                if (!cfg.is_shadow_cluster (cfg))
                {
                  l5 ([&]{trace << "re-evaluating dependent "
                                << pkg.available_name_version_db ()
                                << " involves negotiated configurations and "
                                << "results in " << cfg << ", throwing "
                                << "merge_configuration";});

                  throw merge_configuration {cfg.depth};
                }
                else
                {
                  l5 ([&]{trace << "merge configuration cycle detected for "
                                << "being re-evaluated dependent "
                                << pkg.available_name_version_db ()
                                << " since " << cfg << " is a shadow of itself"
                                << ", throwing merge_configuration_cycle";});

                  throw merge_configuration_cycle {cfg.depth};
                }
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
              //   false   -- some non or being negotiated clusters are merged
              //   true    -- no clusters are merged or all merged have been
              //              negotiated
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

              // Note that there can be some non-negotiated clusters which
              // have been merged based on the shadow cluster into the
              // resulting (being) negotiated cluster. If we had negotiated
              // such non-negotiated clusters normally, we would query
              // existing dependents for the dependencies they contain and
              // consider them in the negotiation process by re-evaluating
              // them (see collect_build_postponed() for details). But if we
              // force-merge a non-negotiated cluster into the (being)
              // negotiated cluster then the existing dependents of its
              // dependencies won't participate in the negotiation, unless we
              // take care of that now. We will recognize such dependencies as
              // not yet (being) recursively collected and re-collect their
              // existing dependents, if any.
              //
              vector<existing_dependent> depts;
              string deps_trace;

              for (const package_key& d: cfg.dependencies)
              {
                build_package* p (entered_build (d));

                // Must be collected at least non-recursively.
                //
                assert (p != nullptr);

                if (p->recursive_collection)
                  continue;

                bool add_deps_trace (verb >= 5);

                for (existing_dependent& ed:
                       query_existing_dependents (trace,
                                                  options,
                                                  d.db,
                                                  d.name,
                                                  false /* exclude_optional */,
                                                  fdb,
                                                  rpt_depts,
                                                  replaced_vers))
                {
                  if (add_deps_trace)
                  {
                    deps_trace += p->available_name_version_db () + ' ';

                    // Make sure the dependency is only listed once in the
                    // trace record.
                    //
                    add_deps_trace = false;
                  }

                  // Add the existing dependent to the list, suppressing
                  // duplicates.
                  //
                  if (find_if (depts.begin (), depts.end (),
                               [&ed] (const existing_dependent& d)
                               {
                                 return d.selected->name == ed.selected->name &&
                                        d.db == ed.db;
                               }) == depts.end ())
                  {
                    depts.push_back (move (ed));
                  }
                }
              }

              if (!depts.empty ())
              {
                l5 ([&]{trace << "cfg-postponing dependent "
                              << pkg.available_name_version_db ()
                              << " adds not (being) collected dependencies "
                              << deps_trace << "with not (being) collected "
                              << "existing dependents to (being) negotiated "
                              << "cluster and results in " << cfg
                              << ", throwing recollect_existing_dependents";});

                // Don't print the "while satisfying..." chain.
                //
                dep_chain.clear ();

                throw recollect_existing_dependents {cfg.depth, move (depts)};
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
                // reset that (see the similar code in
                // collect_build_postponed()).
                //
                small_vector<reference_wrapper<package_skeleton>, 1> depcs;
                forward_list<package_skeleton> depcs_storage; // Ref stability.
                {
                  depcs.reserve (cfg_deps.size ());
                  for (const package_key& pk: cfg_deps)
                  {
                    build_package* b (entered_build (pk));
                    assert (b != nullptr);

                    optional<package_skeleton>& ps (b->skeleton);

                    // If the dependency's skeleton is already present, then
                    // this dependency's configuration has already been
                    // initially negotiated (see collect_build_postponed() for
                    // details) and will now be be up-negotiated. Thus, in
                    // particular, the skeleton must not have the old
                    // configuration dependent variables be loaded.
                    //
                    assert (!ps ||
                            (ps->load_config_flags &
                             package_skeleton::load_config_dependent) == 0);

                    package_skeleton* depc;
                    if (b->recursive_collection)
                    {
                      assert (ps);

                      depcs_storage.push_front (*ps);
                      depc = &depcs_storage.front ();
                      depc->reset ();
                    }
                    else
                      depc = &(ps
                               ? *ps
                               : b->init_skeleton (options,
                                                   false /* load_old_dependent_config */));

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

                assert (b->skeleton); // Should have been init'ed above.

                package_skeleton& ps (*b->skeleton);

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
                    const package_configuration& pc (
                      cfg.dependency_configurations[p]);

                    pair<bool, string> pr (ps.available != nullptr
                                           ? ps.verify_sensible (pc)
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

                    ps.dependent_config (pc);
                  }

                  collect_build_prerequisites (options,
                                               *b,
                                               dep_chain,
                                               fdb,
                                               apc,
                                               rpt_depts,
                                               replaced_vers,
                                               postponed_repo,
                                               postponed_alts,
                                               0 /* max_alt_index */,
                                               postponed_recs,
                                               postponed_edeps,
                                               postponed_deps,
                                               postponed_cfgs,
                                               unacceptable_alts,
                                               unsatisfied_depts);
                }
                else
                  l5 ([&]{trace << "dependency "
                                << b->available_name_version_db ()
                                << " of dependent "
                                << pkg.available_name_version_db ()
                                << " is already (being) recursively "
                                << "collected, skipping";});

                // Unless the dependency collection has been postponed or it
                // is already being reconfigured, reconfigure it if its
                // configuration changes.
                //
                if (!b->recursive_collection_postponed () && !b->reconfigure ())
                {
                  const shared_ptr<selected_package>& sp (b->selected);

                  if (sp != nullptr                          &&
                      sp->state == package_state::configured &&
                      sp->config_checksum != ps.config_checksum ())
                  {
                    b->flags |= build_package::adjust_reconfigure;
                  }
                }
              }

              return true;
            }
          }

          return true;
        };

      // Select a dependency alternative, copying it alone into the resulting
      // dependencies list and evaluating its reflect clause, if present. In
      // the pre-reevaluation mode update the variable prefixes list, if the
      // selected alternative has config clause, and the pre-reevaluation
      // resulting information (re-evaluation position, etc).
      //
      // Note that prebuilds are only used in the pre-reevaluation mode.
      //
      bool selected (false);
      auto select = [&sdeps, &salts, &sdas,
                     &skel,
                     di,
                     &selected,
                     pre_reeval, &banned_var_prefixes, &references_banned_var,
                     &orig_dep,
                     &r] (const dependency_alternative& da,
                          size_t dai,
                          prebuilds&& pbs)
      {
        assert (sdas.empty ());

        if (pre_reeval)
        {
          pair<size_t, size_t> pos (di + 1, dai + 1);

          bool contains_orig_dep (
            find_if (pbs.begin (), pbs.end (),
                     [&orig_dep] (const prebuild& pb)
                     {
                       return pb.dependency.name == orig_dep->name &&
                              pb.db == orig_dep->db;
                     }) != pbs.end ());

          // If the selected alternative contains the originating dependency,
          // then set the originating dependency position, unless it is
          // already set (note that the same dependency package may
          // potentially be specified in multiple depends clauses).
          //
          if (contains_orig_dep && r.originating_dependency_position.first == 0)
            r.originating_dependency_position = pos;

          if (da.prefer || da.require)
          {
            if (contains_orig_dep)
              r.reevaluation_optional = false;

            // If this is the first selected alternative with the config
            // clauses, then save its position and the dependency packages.
            //
            if (r.reevaluation_position.first == 0)
            {
              r.reevaluation_position = pos;

              for (prebuild& pb: pbs)
                r.reevaluation_dependencies.emplace_back (
                  pb.db, move (pb.dependency.name));
            }

            // Save the variable prefixes for the selected alternative
            // dependencies, if we still track them.
            //
            if (r.reevaluation_optional)
            {
              for (const dependency& d: da)
                banned_var_prefixes.push_back (
                  "config." + d.name.variable () + '.');
            }
          }
        }

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
        {
          if (pre_reeval              &&
              r.reevaluation_optional &&
              references_banned_var (*da.reflect))
          {
            r.reevaluation_optional = false;
          }

          skel.evaluate_reflect (*da.reflect, make_pair (di, dai));
        }

        selected = true;
      };

      // Postpone the prerequisite builds collection, optionally inserting the
      // package to the postponements set (can potentially already be there)
      // and saving the enabled alternatives.
      //
      auto postpone = [&pkg, &edas, &postponed] (postponed_packages* postpones)
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

      // During the dependent (pre-)re-evaluation we always try to reproduce
      // the existing setup.
      //
      assert ((!reeval && !pre_reeval) || prereqs != nullptr);

      // Initially try to select an alternative checking that all the
      // constraints imposed by the being built dependents of the dependencies
      // in the alternative are satisfied. Failed that, re-try but this time
      // disable this check so that the unsatisfactory dependency can be
      // properly handled by collect_build() (which can fail, postpone
      // failure, etc; see its implementation for details).
      //
      bool check_constraints (true);

      // Initially don't ignore the unsatisfactory user-specified dependency
      // specs, considering the dependency alternative as unsatisfactory if
      // there are any. Failed that, re-try but this time ignore such specs,
      // so that the unsatisfactory dependency can later be handled by
      // collect_build() (which can fail, postpone failure, etc; see its
      // implementation for details).
      //
      // The thinking here is that we don't ignore the unsatisfactory
      // dependency specs initially to skip the alternatives which are
      // unresolvable for that reason and prefer alternatives which satisfy
      // the command line constraints.
      //
      bool ignore_unsatisfactory_dep_spec (false);

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

        // If true, then some alternatives with unsatisfactory dependencies
        // are detected and, unless the alternative is selected or the
        // selection is postponed, we should re-try with the constraints check
        // disabled (see above for details).
        //
        bool unsatisfactory (false);

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

          precollect_result pcr (
            precollect (da,
                        das.buildtime,
                        prereqs,
                        check_constraints,
                        ignore_unsatisfactory_dep_spec));

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
          if (!pcr.builds)
          {
            if (pcr.repo_postpone)
            {
              if (reeval)
                fail_reeval ();

              postpone (nullptr); // Already inserted into postponed_repo.
              break;
            }

            // If this alternative is reused but is not satisfactory, then
            // switch to the reused-only mode.
            //
            if (pcr.unsatisfactory)
            {
              unsatisfactory = true;

              if (pcr.reused)
                reused_only = true;
            }

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
            first_alt = make_pair (i, move (pcr));
            continue;
          }

          // Try to collect and then select a true alternative, returning true
          // if the alternative is selected or the collection is postponed.
          // Return false if the alternative is ignored (not postponed and not
          // all of it dependencies are reused).
          //
          auto try_select = [postponed_alts, &max_alt_index,
                             &edas, &pkg,
                             di,
                             &prereqs,
                             &check_constraints,
                             &ignore_unsatisfactory_dep_spec,
                             pre_reeval,
                             reeval,
                             &trace,
                             &postpone,
                             &collect,
                             &select] (size_t index, precollect_result&& pcr)
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
            if (pcr.reused)
            {
              // On the diagnostics run there shouldn't be any alternatives
              // that we could potentially select.
              //
              assert (postponed_alts != nullptr);

              if (pre_reeval)
              {
                size_t ni (dai + 1);
                size_t oi (pkg.selected->dependency_alternatives[di]);

                if (ni != oi)
                {
                  l5 ([&]{trace << "re-evaluation of dependent "
                                << pkg.available_name_version_db ()
                                << " deviated at depends clause " << di + 1
                                << ": selected alternative changed to " << ni
                                << " from " << oi;});

                  throw reevaluation_deviated ();
                }
              }
              else if (!collect (da,
                                 dai,
                                 move (*pcr.builds),
                                 prereqs,
                                 check_constraints,
                                 ignore_unsatisfactory_dep_spec))
              {
                postpone (nullptr); // Already inserted into postponed_cfgs.
                return true;
              }

              select (da, dai, move (*pcr.builds));

              // Make sure no more true alternatives are selected during this
              // function call unless we are (pre-)reevaluating a dependent.
              //
              if (!reeval && !pre_reeval)
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

          if (try_select (i, move (pcr)))
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

          precollect_result& pcr (first_alt->second);

          assert (pcr.builds);

          if (pcr.reused || !reused_only)
          {
            // If there are any unacceptable alternatives, then the remaining
            // one should be reused.
            //
            assert (!unacceptable || pcr.reused);

            const auto& eda (edas[first_alt->first]);
            const dependency_alternative& da (eda.first);
            size_t dai (eda.second);

            if (pre_reeval)
            {
              size_t ni (dai + 1);
              size_t oi (sp->dependency_alternatives[di]);

              if (ni != oi)
              {
                l5 ([&]{trace << "re-evaluation of dependent "
                              << pkg.available_name_version_db ()
                              << " deviated for depends clause " << di + 1
                              << ": selected alternative (single) changed to "
                              << ni << " from " << oi;});

                  throw reevaluation_deviated ();
              }
            }
            else if (!collect (da,
                               dai,
                               move (*pcr.builds),
                               prereqs,
                               check_constraints,
                               ignore_unsatisfactory_dep_spec))
            {
              postpone (nullptr); // Already inserted into postponed_cfgs.
              break;
            }

            select (da, dai, move (*pcr.builds));
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
          if (pre_reeval)
          {
            size_t oi (sp->dependency_alternatives[di]);

            l5 ([&]{trace << "re-evaluation of dependent "
                          << pkg.available_name_version_db ()
                          << " deviated for depends clause " << di + 1
                          << ": now cannot select alternative, previously "
                          << oi << " was selected";});

            throw reevaluation_deviated ();
          }

          if (reeval)
            fail_reeval ();

          prereqs = nullptr;
          continue;
        }

        // Retry with the constraints check disabled, if an alternative with
        // the unsatisfactory dependencies is detected.
        //
        if (check_constraints && unsatisfactory)
        {
          check_constraints = false;
          continue;
        }

        if (!ignore_unsatisfactory_dep_spec)
        {
          ignore_unsatisfactory_dep_spec = true;
          continue;
        }

        // Otherwise we would have thrown/failed earlier.
        //
        assert (!pre_reeval && !reeval);

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
          {
            precollect (da.first,
                        das.buildtime,
                        nullptr /* prereqs */,
                        true /* check_constraints */,
                        false /* ignore_unsatisfactory_dep_spec */,
                        &dr);
          }

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
          // Note that we pass false as the check_constraints argument to make
          // sure that the alternatives are always saved into
          // precollect_result::builds rather than into
          // precollect_result::unsatisfactory.
          //
          precollect_result r (
            precollect (da.first,
                        das.buildtime,
                        nullptr /* prereqs */,
                        false /* check_constraints */,
                        true /* ignore_unsatisfactory_dep_spec */));

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
              precollect (da.first,
                          das.buildtime,
                          nullptr /* prereqs */,
                          true /* check_constraints */,
                          false /* ignore_unsatisfactory_dep_spec */));

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
                          true /* check_constraints */,
                          false /* ignore_unsatisfactory_dep_spec */,
                          &dr);
            }
          }
        }
      }

      // Bail out if the collection is postponed.
      //
      // Note that it's tempting to also bail out in the pre-reevaluation mode
      // if we have already collected all the required resulting information
      // (reevaluation position, originating dependency position, etc).
      // However, in this case we may not detect the dependent deviation and
      // thus we always iterate through all the depends clauses.
      //
      if (postponed)
        break;
    }

    if (reeval)
    {
      if (!reevaluated)
        fail_reeval ();

      assert (postponed);
    }

    if (pre_reeval)
    {
      // It doesn't feel like it may happen in the pre-reevaluation mode. If
      // it still happens, maybe due to some manual tampering, let's assume
      // this as a deviation case.
      //
      if (r.originating_dependency_position.first == 0)
      {
        l5 ([&]{trace << "re-evaluation of dependent "
                      << pkg.available_name_version_db ()
                      << " deviated: previously selected dependency "
                      << *orig_dep << " is not selected anymore";});

        throw reevaluation_deviated ();
      }

      l5 ([&]
          {
            diag_record dr (trace);
            dr << "pre-reevaluated " << pkg.available_name_version_db ()
               << ": ";

            pair<size_t, size_t> pos (r.reevaluation_position);

            if (pos.first != 0)
            {
              dr << pos.first << ',' << pos.second;

              if (r.reevaluation_optional)
                dr << " re-evaluation is optional";
            }
            else
              dr << "end reached";
          });
    }
    else
    {
      dep_chain.pop_back ();

      l5 ([&]{trace << (!postponed ? "end "          :
                        reeval     ? "re-evaluated " :
                                     "postpone ")
                    << pkg.available_name_version_db ();});
    }

    return pre_reeval && r.reevaluation_position.first != 0
           ? move (r)
           : optional<pre_reevaluate_result> ();
  }

  void build_packages::
  collect_build_prerequisites (const pkg_build_options& o,
                               database& db,
                               const package_name& name,
                               const function<find_database_function>& fdb,
                               const function<add_priv_cfg_function>& apc,
                               const repointed_dependents& rpt_depts,
                               replaced_versions& replaced_vers,
                               postponed_packages& postponed_repo,
                               postponed_packages& postponed_alts,
                               size_t max_alt_index,
                               postponed_packages& postponed_recs,
                               postponed_existing_dependencies& postponed_edeps,
                               postponed_dependencies& postponed_deps,
                               postponed_configurations& postponed_cfgs,
                               unacceptable_alternatives& unacceptable_alts,
                               unsatisfied_dependents& unsatisfied_depts)
  {
    auto mi (map_.find (db, name));
    assert (mi != map_.end ());

    build_package_refs dep_chain;

    collect_build_prerequisites (o,
                                 mi->second.package,
                                 dep_chain,
                                 fdb,
                                 apc,
                                 rpt_depts,
                                 replaced_vers,
                                 &postponed_repo,
                                 &postponed_alts,
                                 max_alt_index,
                                 postponed_recs,
                                 postponed_edeps,
                                 postponed_deps,
                                 postponed_cfgs,
                                 unacceptable_alts,
                                 unsatisfied_depts);
  }

  void build_packages::
  collect_repointed_dependents (
    const pkg_build_options& o,
    const repointed_dependents& rpt_depts,
    replaced_versions& replaced_vers,
    postponed_packages& postponed_repo,
    postponed_packages& postponed_alts,
    postponed_packages& postponed_recs,
    postponed_existing_dependencies& postponed_edeps,
    postponed_dependencies& postponed_deps,
    postponed_configurations& postponed_cfgs,
    unacceptable_alternatives& unacceptable_alts,
    unsatisfied_dependents& unsatisfied_depts,
    const function<find_database_function>& fdb,
    const function<add_priv_cfg_function>& apc)
  {
    tracer trace ("collect_repointed_dependents");

    for (const auto& rd: rpt_depts)
    {
      database&           db (rd.first.db);
      const package_name& nm (rd.first.name);

      {
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
      }

      shared_ptr<selected_package> sp (db.load<selected_package> (nm));

      // The repointed dependent can be an orphan, so just create the
      // available package from the selected package.
      //
      auto rp (make_available_fragment (o, db, sp));

      // Add the prerequisite replacements as the required-by packages.
      //
      set<package_version_key> required_by;
      for (const auto& prq: rd.second)
      {
        if (prq.second) // Prerequisite replacement?
        {
          const package_key& pk (prq.first);

          // Note that the dependency can potentially be just pre-entered, in
          // which case its version is not known at this point.
          //
          assert (entered_build (pk) != nullptr);

          required_by.emplace (pk.db, pk.name, version ());
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
        nullopt,                    // Upgrade.
        false,                      // Deorphan.
        move (required_by),         // Required by (dependencies).
        false,                      // Required by dependents.
        build_package::adjust_reconfigure | build_package::build_repoint};

      build_package_refs dep_chain;

      package_key pk {db, nm};

      // Note that the repointed dependent can well be a dependency whose
      // recursive processing should be postponed.
      //
      auto i (postponed_deps.find (pk));
      if (i != postponed_deps.end ())
      {
        // Note that here we would collect the repointed dependent recursively
        // without specifying any configuration for it.
        //
        i->second.wout_config = true;

        // Note: not recursive.
        //
        collect_build (
          o, move (p), replaced_vers, postponed_cfgs, unsatisfied_depts);

        l5 ([&]{trace << "dep-postpone repointed dependent " << pk;});
      }
      else
      {
        const postponed_configuration* pcfg (
          postponed_cfgs.find_dependency (pk));

        if (pcfg != nullptr)
        {
          // Note: not recursive.
          //
          collect_build (
            o, move (p), replaced_vers, postponed_cfgs, unsatisfied_depts);

          l5 ([&]{trace << "dep-postpone repointed dependent " << pk
                        << " since already in cluster " << *pcfg;});
        }
        else
        {
          build_package_refs dep_chain;

          // Note: recursive.
          //
          collect_build (o,
                         move (p),
                         replaced_vers,
                         postponed_cfgs,
                         unsatisfied_depts,
                         &dep_chain,
                         fdb,
                         apc,
                         &rpt_depts,
                         &postponed_repo,
                         &postponed_alts,
                         &postponed_recs,
                         &postponed_edeps,
                         &postponed_deps,
                         &unacceptable_alts);
        }
      }
    }
  }

  void build_packages::
  collect_drop (const pkg_build_options&,
                database& db,
                shared_ptr<selected_package> sp,
                replaced_versions& replaced_vers)
  {
    tracer trace ("collect_drop");

    package_key pk (db, sp->name);

    // If there is an entry for building specific version of the package (the
    // available member is not NULL), then it wasn't created to prevent our
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
      nullopt,    // Upgrade.
      false,      // Deorphan.
      {},         // Required by.
      false,      // Required by dependents.
      0};         // State flags.

    auto i (map_.find (pk));

    if (i != map_.end ())
    {
      build_package& bp (i->second.package);

      // Don't overwrite the build object if its required-by package names
      // have the "required by dependents" semantics.
      //
      if (!bp.required_by_dependents)
      {
        if (bp.available != nullptr)
        {
          // Similar to the version replacement in collect_build(), see if
          // in-place drop is possible (no dependencies, etc) and set scratch
          // to false if that's the case.
          //
          bool scratch (true);

          // While checking if the package has any dependencies skip the
          // toolchain build-time dependencies since they should be quite
          // common.
          //
          // An update: it turned out that just absence of dependencies is not
          // the only condition that causes a package to be dropped in place.
          // The following conditions must also be met:
          //
          // - The package must also not participate in any configuration
          //   negotiation on the dependency side (otherwise it could have
          //   been added to a cluster as a dependency).
          //
          // - The package must not be added to unsatisfied_depts on the
          //   dependency side.
          //
          // This feels quite hairy at the moment, so we won't be dropping in
          // place for now.
          //
#if 0
          if (!has_dependencies (options, bp.available->dependencies))
            scratch = false;
#endif

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

        // Overwrite the existing (possibly pre-entered, adjustment, or
        // repoint) entry.
        //
        l4 ([&]{trace << "overwrite " << pk;});

        bp = move (p);
      }
      else
      {
        assert (!bp.required_by.empty ());

        l5 ([&]
            {
              diag_record dr (trace);
              dr << pk << " cannot be dropped since it is required by ";
              for (auto b (bp.required_by.begin ()), i (b);
                   i != bp.required_by.end ();
                   ++i)
                dr << (i != b ? ", " : "") << *i;
            });
      }
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
        nullopt,       // Upgrade.
        false,         // Deorphan.
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
                           postponed_packages& postponed_recs,
                           postponed_existing_dependencies& postponed_edeps,
                           postponed_dependencies& postponed_deps,
                           postponed_configurations& postponed_cfgs,
                           strings& postponed_cfgs_history,
                           unacceptable_alternatives& unacceptable_alts,
                           unsatisfied_dependents& unsatisfied_depts,
                           const function<find_database_function>& fdb,
                           const repointed_dependents& rpt_depts,
                           const function<add_priv_cfg_function>& apc,
                           postponed_configuration* pcfg)
  {
    // NOTE: enable and run the tests with the config.bpkg.tests.all=true
    //       variable if changing anything in this function.
    //

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
                const postponed_packages& postponed_recs,
                const replaced_versions& replaced_vers,
                const postponed_existing_dependencies& postponed_edeps,
                const postponed_dependencies& postponed_deps,
                const postponed_configurations& postponed_cfgs,
                const unsatisfied_dependents& unsatisfied_depts)
          : pkgs_ (pkgs),
            replaced_vers_ (replaced_vers),
            postponed_edeps_ (postponed_edeps),
            postponed_deps_ (postponed_deps),
            postponed_cfgs_ (postponed_cfgs),
            unsatisfied_depts_ (unsatisfied_depts)
      {
        auto save = [] (vector<package_key>& d, const postponed_packages& s)
        {
          d.reserve (s.size ());

          for (const build_package* p: s)
            d.emplace_back (p->db, p->name ());
        };

        save (postponed_repo_, postponed_repo);
        save (postponed_alts_, postponed_alts);
        save (postponed_recs_, postponed_recs);
      }

      void
      restore (build_packages& pkgs,
               postponed_packages& postponed_repo,
               postponed_packages& postponed_alts,
               postponed_packages& postponed_recs,
               replaced_versions& replaced_vers,
               postponed_existing_dependencies& postponed_edeps,
               postponed_dependencies& postponed_deps,
               postponed_configurations& postponed_cfgs,
               unsatisfied_dependents& unsatisfied_depts)
      {
        pkgs            = move (pkgs_);
        replaced_vers   = move (replaced_vers_);
        postponed_cfgs  = move (postponed_cfgs_);
        postponed_deps  = move (postponed_deps_);
        postponed_edeps = move (postponed_edeps_);

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
        restore (postponed_recs, postponed_recs_);

        unsatisfied_depts = move (unsatisfied_depts_);
      }

    private:
      // Note: try to use vectors instead of sets for storage to save
      //       memory. We could probably optimize this some more if necessary
      //       (there are still sets/maps inside).
      //
      build_packages                  pkgs_;
      vector<package_key>             postponed_repo_;
      vector<package_key>             postponed_alts_;
      vector<package_key>             postponed_recs_;
      replaced_versions               replaced_vers_;
      postponed_existing_dependencies postponed_edeps_;
      postponed_dependencies          postponed_deps_;
      postponed_configurations        postponed_cfgs_;
      unsatisfied_dependents          unsatisfied_depts_;
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

      // Re-evaluate existing dependents for dependencies in this
      // configuration cluster. Omit dependents which are already being built,
      // dropped, or postponed.
      //
      // Note that the existing dependent can be re-evaluated to an earlier
      // position than the position of the dependency which has introduced
      // this existing dependent. Thus, re-evaluating such a dependent does
      // not necessarily add this dependent together with the dependencies at
      // the re-evaluation target position specifically to this cluster. We,
      // however, re-evaluate all the discovered existing dependents. Also
      // note that these dependents will be added to their respective clusters
      // with the `existing` flag as a part of the dependents' re-evaluation
      // (see the collect lambda in collect_build_prerequisites() for
      // details).
      //
      // After being re-evaluated the existing dependents are recursively
      // collected in the same way and at the same time as the new dependents
      // of the clusters they belong to.
      //
      // Note that some of the postponed existing dependents may already be in
      // the cluster. Thus, collect the postponed existing dependents to omit
      // them from the configuration negotiation and from the subsequent
      // recursive collection. Note that we will up-negotiate the
      // configuration these dependents apply to their dependencies after
      // these dependents will be collected via their own dependents with the
      // configuration clauses.
      //
      set<package_key> postponed_existing_dependents;
      {
        // Map existing dependents to the dependencies they apply a
        // configuration to. Also, collect the information which is required
        // for a dependent re-evaluation (selected package, etc).
        //
        // Note that we may end up adding additional dependencies to
        // pcfg->dependencies which in turn may have additional existing
        // dependents which we need to process. Feels like doing this
        // iteratively is the best option.
        //
        // Also note that we need to make sure we don't re-process the same
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

            for (existing_dependent& ed:
                   query_existing_dependents (trace,
                                              o,
                                              p.db,
                                              p.name,
                                              false /* exclude_optional */,
                                              fdb,
                                              rpt_depts,
                                              replaced_vers))
            {
              if (ed.dependency)
              {
                package_key pk (ed.db, ed.selected->name);

                // If this dependent is present in postponed_deps or in some
                // cluster as a dependency, then it means that someone depends
                // on it with configuration and it's no longer considered an
                // existing dependent (it will be reconfigured). However, this
                // fact may not be reflected yet. And it can actually turn out
                // bogus.
                //
                auto pi (postponed_deps.find (pk));
                if (pi != postponed_deps.end ())
                {
                  l5 ([&]{trace << "skip dep-postponed existing dependent "
                                << pk << " of dependency " << p;});

                  // Note that here we would re-evaluate the existing
                  // dependent without specifying any configuration for it.
                  //
                  pi->second.wout_config = true;

                  collect_existing_dependent (o,
                                              ed,
                                              {p},
                                              replaced_vers,
                                              postponed_cfgs,
                                              unsatisfied_depts);

                  postponed_existing_dependents.insert (pk);
                  continue;
                }

                const postponed_configuration* pcfg (
                  postponed_cfgs.find_dependency (pk));

                if (pcfg != nullptr)
                {
                  l5 ([&]{trace << "skip existing dependent " << pk
                                << " of dependency " << p << " since "
                                << "dependent already in cluster " << *pcfg
                                << " (as a dependency)";});

                  postponed_existing_dependents.insert (pk);
                  continue;
                }

                auto i (dependents.find (pk));

                // If the existing dependent is not in the map yet, then add
                // it.
                //
                if (i == dependents.end ())
                {
                  if (*ed.dependency != p)
                    collect_existing_dependent_dependency (o,
                                                           ed,
                                                           replaced_vers,
                                                           postponed_cfgs,
                                                           unsatisfied_depts);

                  i = dependents.emplace (
                    move (pk), existing_dependent_ex (move (ed))).first;
                }
                else
                {
                  // We always re-evaluate to the earliest position.
                  //
                  assert (i->second.dependency_position ==
                          ed.dependency_position);
                }

                // Note that we add here the dependency which introduced this
                // existing dependent, rather than the dependency which
                // position we re-evaluate to, and which we want to be
                // mentioned in the plan, if printed.
                //
                i->second.dependencies.push_back (p);
              }
              else
              {
                l5 ([&]{trace << "schedule re-collection of deviated "
                              << "existing dependent " << *ed.selected
                              << ed.db;});

                recollect_existing_dependent (o,
                                              ed,
                                              replaced_vers,
                                              postponed_recs,
                                              postponed_cfgs,
                                              unsatisfied_depts,
                                              true /* add_required_by */);
              }
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

              // Note that a re-evaluated package doesn't necessarily needs to
              // be reconfigured and thus we don't add the
              // build_package::adjust_reconfigure flag here.
              //
              // Specifically, if none of its dependencies get reconfigured,
              // then it doesn't need to be reconfigured either since nothing
              // changes for its config clauses. Otherwise, the
              // build_package::adjust_reconfigure flag will be added normally
              // by collect_dependents().
              //
              collect_existing_dependent (o,
                                          ed,
                                          move (ed.dependencies),
                                          replaced_vers,
                                          postponed_cfgs,
                                          unsatisfied_depts);

              build_package* b (entered_build (d.first));
              assert (b != nullptr);

              // Re-evaluate up to the earliest position.
              //
              assert (ed.dependency_position.first != 0);

              try
              {
                build_package_refs dep_chain;
                collect_build_prerequisites (o,
                                             *b,
                                             dep_chain,
                                             fdb,
                                             apc,
                                             rpt_depts,
                                             replaced_vers,
                                             &postponed_repo,
                                             &postponed_alts,
                                             numeric_limits<size_t>::max (),
                                             postponed_recs,
                                             postponed_edeps,
                                             postponed_deps,
                                             postponed_cfgs,
                                             unacceptable_alts,
                                             unsatisfied_depts,
                                             ed.dependency_position);
              }
              catch (const merge_configuration_cycle& e)
              {
                l5 ([&]{trace << "re-evaluation of existing dependent "
                              << b->available_name_version_db () << " failed "
                              << "due to merge configuration cycle for "
                              << *pcfg << ", throwing "
                              << "recollect_existing_dependents";});

                throw recollect_existing_dependents {e.depth, {move (ed)}};
              }

              ed.reevaluated = true;
            }
          }
        }
      }

      // Negotiate the configuration.
      //
      // The overall plan is as follows: continue refining the configuration
      // until there are no more changes by giving each dependent a chance to
      // make further adjustments.
      //
      l5 ([&]{trace << "cfg-negotiate begin " << *pcfg;});

      // For the cluster's dependencies, the skeleton should not be present
      // since we haven't yet started recursively collecting them. And we
      // couldn't have started collecting them before we negotiated their
      // configurations (that's in contrast to the up-negotiation). Let's
      // assert for that here to make sure that's also true for dependencies
      // of the postponed existing dependents of this cluster.
      //
#ifndef NDEBUG
      for (const package_key& p: pcfg->dependencies)
      {
        build_package* b (entered_build (p));
        assert (b != nullptr && !b->skeleton && !b->recursive_collection);
      }
#endif

      for (auto b (pcfg->dependents.begin ()),
                i (b),
                e (pcfg->dependents.end ()); i != e; )
      {
        if (postponed_existing_dependents.find (i->first) !=
            postponed_existing_dependents.end ())
        {
          l5 ([&]{trace << "skip dep-postponed existing dependent "
                        << i->first;});

          ++i;
          continue;
        }

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

        // If a dependency has already been recursively collected, then we can
        // no longer call reload_defaults() or verify_sensible() on its
        // skeleton. Thus, we make a temporary copy and reset that (see the
        // collect() lambda in collect_build_prerequisites() for more
        // details).
        //
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

            // Shouldn't be here otherwise.
            //
            assert (b != nullptr && !b->recursive_collection);

            package_skeleton* depc (
              &(b->skeleton
                ? *b->skeleton
                : b->init_skeleton (o,
                                    false /* load_old_dependent_config */)));

            depcs.push_back (*depc);
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

        // Skip the dependencies which are already collected recursively.
        //
        if (!b->recursive_collection)
        {
          // Note that due to the existing dependents postponement some of the
          // dependencies may have no dependent configuration applied to them
          // at this time. In this case such dependencies may have no skeleton
          // yet and thus we initialize it. Note that we will still apply the
          // empty configuration to such dependencies and collect them
          // recursively, since the negotiation machinery relies on the fact
          // that the dependencies of a negotiated cluster are (being)
          // recursively collected. When the time comes and such a dependency
          // is collected via its (currently postponed) existing dependent,
          // then its configuration will be up-negotiated (likely involving
          // throwing the retry_configuration exception).
          //
          if (!b->skeleton)
            b->init_skeleton (o, false /* load_old_dependent_config */);

          package_skeleton& ps (*b->skeleton);

          // Verify and set the dependent configuration for this dependency.
          //
          // Note: see similar code for the up-negotiation case.
          //
          {
            const package_configuration& pc (
              pcfg->dependency_configurations[p]);

            // Skip the verification if this is a system package without
            // skeleton info.
            //
            pair<bool, string> pr (ps.available != nullptr
                                   ? ps.verify_sensible (pc)
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

            ps.dependent_config (pc);
          }

          build_package_refs dep_chain;
          collect_build_prerequisites (o,
                                       *b,
                                       dep_chain,
                                       fdb,
                                       apc,
                                       rpt_depts,
                                       replaced_vers,
                                       &postponed_repo,
                                       &postponed_alts,
                                       0 /* max_alt_index */,
                                       postponed_recs,
                                       postponed_edeps,
                                       postponed_deps,
                                       postponed_cfgs,
                                       unacceptable_alts,
                                       unsatisfied_depts);
        }
        else
          l5 ([&]{trace << "dependency " << b->available_name_version_db ()
                        << " is already (being) recursively collected, "
                        << "skipping";});

        // Unless the dependency collection has been postponed or it is
        // already being reconfigured, reconfigure it if its configuration
        // changes.
        //
        if (!b->recursive_collection_postponed () && !b->reconfigure ())
        {
          const shared_ptr<selected_package>& sp (b->selected);

          assert (b->skeleton); // Should have been init'ed above.

          package_skeleton& ps (*b->skeleton);

          if (sp != nullptr                          &&
              sp->state == package_state::configured &&
              sp->config_checksum != ps.config_checksum ())
          {
            b->flags |= build_package::adjust_reconfigure;
          }
        }
      }

      // Continue processing dependents with this config.
      //
      l5 ([&]{trace << "recursively collect cfg-negotiated dependents";});

      for (const auto& p: dependents)
      {
        if (postponed_existing_dependents.find (p) !=
            postponed_existing_dependents.end ())
        {
          l5 ([&]{trace << "skip dep-postponed existing dependent " << p;});
          continue;
        }

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
                                     dep_chain,
                                     fdb,
                                     apc,
                                     rpt_depts,
                                     replaced_vers,
                                     &postponed_repo,
                                     &postponed_alts,
                                     0 /* max_alt_index */,
                                     postponed_recs,
                                     postponed_edeps,
                                     postponed_deps,
                                     postponed_cfgs,
                                     unacceptable_alts,
                                     unsatisfied_depts);
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

    for (bool prog (find_if (postponed_recs.begin (), postponed_recs.end (),
                             [] (const build_package* p)
                             {
                               // Note that we check for the dependencies
                               // presence rather than for the
                               // recursive_collection flag (see below for
                               // details).
                               //
                               return !p->dependencies;
                             }) != postponed_recs.end () ||
                    !postponed_repo.empty ()             ||
                    !postponed_cfgs.negotiated ()        ||
                    !postponed_alts.empty ()             ||
                    postponed_deps.has_bogus ());
         prog; )
    {
      // First, recursively recollect the not yet collected packages (deviated
      // existing dependents, etc).
      //
      prog = false;

      postponed_packages pcs;
      for (build_package* p: postponed_recs)
      {
        // Note that we check for the dependencies presence rather than for
        // the recursive_collection flag to also recollect the existing
        // dependents which, for example, may have been specified on the
        // command line and whose recursive collection has been pruned since
        // there were no reason to collect it (configured, no upgrade,
        // etc). Also note that this time we expect the collection to be
        // enforced with the build_recollect flag.
        //
        assert ((p->flags & build_package::build_recollect) != 0);

        if (!p->dependencies)
        {
          package_key pk (p->db, p->name ());

          auto pi (postponed_deps.find (pk));
          if (pi != postponed_deps.end ())
          {
            l5 ([&]{trace << "skip re-collection of dep-postponed package "
                          << pk;});

            // Note that here we would re-collect the package without
            // specifying any configuration for it.
            //
            pi->second.wout_config = true;

            continue;
          }
          else
          {
            const postponed_configuration* pcfg (
              postponed_cfgs.find_dependency (pk));

            if (pcfg != nullptr)
            {
              l5 ([&]{trace << "skip re-collection of dep-postponed package "
                            << pk << " since already in cluster " << *pcfg;});

              continue;
            }
          }

          build_package_refs dep_chain;
          collect_build_prerequisites (o,
                                       *p,
                                       dep_chain,
                                       fdb,
                                       apc,
                                       rpt_depts,
                                       replaced_vers,
                                       &postponed_repo,
                                       &postponed_alts,
                                       0 /* max_alt_index */,
                                       pcs,
                                       postponed_edeps,
                                       postponed_deps,
                                       postponed_cfgs,
                                       unacceptable_alts,
                                       unsatisfied_depts);

          // Note that the existing dependent collection can be postponed
          // due to it's own existing dependents.
          //
          if (p->recursive_collection)
          {
            // Must be present since the re-collection is enforced.
            //
            assert (p->dependencies);

            prog = true;
          }
        }
      }

      // Scheduling new packages for re-collection is also a progress.
      //
      if (!prog)
        prog = !pcs.empty ();

      if (prog)
      {
        postponed_recs.insert (pcs.begin (), pcs.end ());
        continue;
      }

      postponed_packages prs;
      postponed_packages pas;

      // Now, as there is no more progress made in recollecting of the not yet
      // collected packages, try to collect the repository-related
      // postponements.
      //
      for (build_package* p: postponed_repo)
      {
        l5 ([&]{trace << "collect rep-postponed "
                      << p->available_name_version_db ();});

        build_package_refs dep_chain;

        collect_build_prerequisites (o,
                                     *p,
                                     dep_chain,
                                     fdb,
                                     apc,
                                     rpt_depts,
                                     replaced_vers,
                                     &prs,
                                     &pas,
                                     0 /* max_alt_index */,
                                     postponed_recs,
                                     postponed_edeps,
                                     postponed_deps,
                                     postponed_cfgs,
                                     unacceptable_alts,
                                     unsatisfied_depts);
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
                      postponed_recs,
                      replaced_vers,
                      postponed_edeps,
                      postponed_deps,
                      postponed_cfgs,
                      unsatisfied_depts);

          try
          {
            collect_build_postponed (o,
                                     replaced_vers,
                                     postponed_repo,
                                     postponed_alts,
                                     postponed_recs,
                                     postponed_edeps,
                                     postponed_deps,
                                     postponed_cfgs,
                                     postponed_cfgs_history,
                                     unacceptable_alts,
                                     unsatisfied_depts,
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
                       postponed_recs,
                       replaced_vers,
                       postponed_edeps,
                       postponed_deps,
                       postponed_cfgs,
                       unsatisfied_depts);

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
                       postponed_recs,
                       replaced_vers,
                       postponed_edeps,
                       postponed_deps,
                       postponed_cfgs,
                       unsatisfied_depts);

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
          catch (const recollect_existing_dependents& e)
          {
            // If this is not "our problem", then keep looking.
            //
            if (e.depth != pcd)
              throw;

            // Restore the state from snapshot.
            //
            // Note: postponed_cfgs is re-assigned.
            //
            s.restore (*this,
                       postponed_repo,
                       postponed_alts,
                       postponed_recs,
                       replaced_vers,
                       postponed_edeps,
                       postponed_deps,
                       postponed_cfgs,
                       unsatisfied_depts);

            pc = &postponed_cfgs[ci];

            assert (!pc->negotiated);

            // Drop any accumulated configuration (which could be carried
            // over from retry_configuration logic).
            //
            pc->dependency_configurations.clear ();

            // The shadow cluster likely contains the problematic
            // dependent/dependencies. Thus, it feels right to drop the shadow
            // before re-negotiating the cluster.
            //
            pc->shadow_cluster.clear ();

            l5 ([&]{trace << "cfg-negotiation of " << *pc << " failed due to "
                          << "some existing dependents related problem, "
                          << "scheduling their re-collection";});

            for (const existing_dependent& ed: e.dependents)
            {
              l5 ([&]{trace << "schedule re-collection of "
                            << (!ed.dependency ? "deviated " : "")
                            << "existing dependent " << *ed.selected
                            << ed.db;});

              // Note that we pass false as the add_required_by argument since
              // the package builds collection state has been restored and the
              // originating dependency for this existing dependent may not be
              // collected anymore.
              //
              recollect_existing_dependent (o,
                                            ed,
                                            replaced_vers,
                                            postponed_recs,
                                            postponed_cfgs,
                                            unsatisfied_depts,
                                            false /* add_required_by */);
            }
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
        // Sort the postponements in the unprocessed dependencies count
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
                                         dep_chain,
                                         fdb,
                                         apc,
                                         rpt_depts,
                                         replaced_vers,
                                         &prs,
                                         &pas,
                                         i,
                                         postponed_recs,
                                         postponed_edeps,
                                         postponed_deps,
                                         postponed_cfgs,
                                         unacceptable_alts,
                                         unsatisfied_depts);

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
            // Note that we don't need to check for new configuration-related
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

      // Note that a bogus dependency postponement may, in particular, happen
      // to an existing dependent due to the cycle introduced by its own
      // existing dependent. For example, an existing dependent (libfoo)
      // re-evaluation can be postponed since it starts a chain of
      // re-evaluations which ends up with its own existing dependent (foo)
      // with config clause, which being collected after re-evaluation is
      // unable to collect the prematurely collected libfoo. In this case
      // postponing collection of libfoo will also prevent foo from being
      // re-evaluated, the postponement will turn out to be bogus, and we may
      // start yo-yoing (see the
      // pkg-build/.../recollect-dependent-bogus-dependency-postponement test
      // for the real example). To prevent that, let's try to collect a
      // postponed bogus dependency by recollecting its existing dependents,
      // if present, prior to considering it as really bogus and re-collecting
      // everything from scratch.
      //
      for (const auto& pd: postponed_deps)
      {
        if (pd.second.bogus ())
        {
          const package_key& pk (pd.first);

          for (existing_dependent& ed:
                 query_existing_dependents (trace,
                                            o,
                                            pk.db,
                                            pk.name,
                                            false /* exclude_optional */,
                                            fdb,
                                            rpt_depts,
                                            replaced_vers))
          {
            l5 ([&]{trace << "schedule re-collection of "
                          << (!ed.dependency ? "deviated " : "")
                          << "existing dependent " << *ed.selected
                          << ed.db << " due to bogus postponement of "
                          << "dependency " << pk;});

            recollect_existing_dependent (o,
                                          ed,
                                          replaced_vers,
                                          postponed_recs,
                                          postponed_cfgs,
                                          unsatisfied_depts,
                                          true /* add_required_by */);
            prog = true;
            break;
          }
        }
      }

      if (prog)
        continue;

      // Finally, erase the bogus postponements and re-collect from scratch,
      // if any (see postponed_dependencies for details).
      //
      // Note that we used to re-collect such postponements in-place but
      // re-doing from scratch feels more correct (i.e., we may end up doing
      // it earlier which will affect dependency alternatives).
      //
      postponed_deps.cancel_bogus (trace);
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
                                   dep_chain,
                                   fdb,
                                   apc,
                                   rpt_depts,
                                   replaced_vers,
                                   nullptr,
                                   nullptr,
                                   0,
                                   postponed_recs,
                                   postponed_edeps,
                                   postponed_deps,
                                   postponed_cfgs,
                                   unacceptable_alts,
                                   unsatisfied_depts);

      assert (false); // Can't be here.
    }

    if (!postponed_alts.empty ())
    {
      build_package_refs dep_chain;

      collect_build_prerequisites (o,
                                   **postponed_alts.begin (),
                                   dep_chain,
                                   fdb,
                                   apc,
                                   rpt_depts,
                                   replaced_vers,
                                   nullptr,
                                   nullptr,
                                   0,
                                   postponed_recs,
                                   postponed_edeps,
                                   postponed_deps,
                                   postponed_cfgs,
                                   unacceptable_alts,
                                   unsatisfied_depts);

      assert (false); // Can't be here.
    }

    // While the assumption is that we shouldn't leave any non-negotiated
    // clusters, let's verify that for good measure. Let's thus trace the
    // non-negotiated clusters before the assertion.
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
         const function<find_database_function>& fdb,
         bool reorder)
  {
    package_refs chain;
    return order (db, name, chain, fdb, reorder);
  }

  set<package_key> build_packages::
  collect_dependents (const repointed_dependents& rpt_depts,
                      unsatisfied_dependents& unsatisfied_depts)
  {
    set<package_key> r;

    // First, cache the packages in the map since we will be adding new
    // entries to the map while collecting dependents of the initial package
    // set, recursively.
    //
    // Note: the pointer is stable (points to a value in std::map).
    //
    vector<build_package*> deps;

    for (auto& p: map_)
    {
      build_package& d (p.second.package);

      // Prune if this is not a configured package being up/down-graded
      // or reconfigured.
      //
      if (d.action && *d.action != build_package::drop && d.reconfigure ())
        deps.push_back (&d);
    }

    // Note: the pointer is stable (see above for details).
    //
    set<const build_package*> visited_deps;

    for (build_package* p: deps)
      collect_dependents (*p, rpt_depts, unsatisfied_depts, visited_deps, r);

    return r;
  }

  void build_packages::
  collect_dependents (build_package& p,
                      const repointed_dependents& rpt_depts,
                      unsatisfied_dependents& unsatisfied_depts,
                      set<const build_package*>& visited_deps,
                      set<package_key>& r)
  {
    tracer trace ("collect_dependents");

    // Bail out if the dependency has already been visited and add it to the
    // visited set otherwise.
    //
    if (!visited_deps.insert (&p).second)
      return;

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
        optional<version_constraint>& dc (pd.constraint);

        auto i (map_.find (ddb, dn));

        // Make sure the up/downgraded package still satisfies this
        // dependent. But first "prune" if the dependent is being dropped or
        // this is a replaced prerequisite of the repointed dependent.
        //
        // Note that the repointed dependents are always collected (see
        // collect_build_prerequisites() for details).
        //
        bool check (ud != 0 && dc);

        if (i != map_.end ())
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
          const version_constraint& c (*dc);

          // If the new dependency version doesn't satisfy the existing
          // dependent, then postpone the failure in the hope that this
          // problem will be resolved naturally (the dependent will also be
          // up/downgraded, etc; see unsatisfied_dependents for details).
          //
          if (!satisfies (av, c))
          {
            package_key d (ddb, dn);

            l5 ([&]{trace << "postpone failure for existing dependent " << d
                          << " unsatisfied with dependency "
                          << p.available_name_version_db () << " ("
                          << c << ')';});

            unsatisfied_depts.add (d, package_key (p.db, p.name ()), c);
          }
        }

        auto adjustment = [&dn, &ddb, &n, &pdb] () -> build_package
        {
          shared_ptr<selected_package> dsp (ddb.load<selected_package> (dn));

          // A system package cannot be a dependent.
          //
          assert (!dsp->system ());

          package_version_key pvk (pdb, n, version ());

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
            nullopt,                   // Upgrade.
            false,                     // Deorphan.
            {move (pvk)},              // Required by (dependency).
            false,                     // Required by dependents.
            build_package::adjust_reconfigure};
        };

        // If the existing entry is pre-entered or is an adjustment, then we
        // merge it into the new adjustment entry. Otherwise (is a build), we
        // just add the reconfigure adjustment flag to it, unless it is
        // already being reconfigured. In the later case we don't add the
        // dependent to the resulting set since we neither add a new entry to
        // the map nor modify an existing one.
        //
        bool add (true);
        if (i != map_.end ())
        {
          build_package& dp (i->second.package);

          if (!dp.action ||                       // Pre-entered.
              *dp.action != build_package::build) // Adjustment.
          {
            build_package bp (adjustment ());
            bp.merge (move (dp));
            dp = move (bp);
          }
          else                                    // Build.
          {
            if (!dp.reconfigure ())
              dp.flags |= build_package::adjust_reconfigure;
            else
              add = false;
          }
        }
        else
        {
          // Don't move dn since it is used by adjustment().
          //
          i = map_.emplace (package_key {ddb, dn},
                            data_type {end (), adjustment ()}).first;
        }

        if (add)
          r.insert (i->first);

        build_package& dp (i->second.package);

        // Add this dependent's constraint, if present, to the dependency's
        // constraints list for completeness, while suppressing duplicates.
        //
        if (dc)
        {
          using constraint_type = build_package::constraint_type;

          // Pre-entered entries are always converted to adjustments (see
          // above).
          //
          assert (dp.action);

          constraint_type c (move (*dc),
                             ddb,
                             move (dn),
                             dp.selected->version,
                             *dp.action != build_package::build);

          if (find_if (p.constraints.begin (), p.constraints.end (),
                       [&c] (const constraint_type& v)
                       {
                         return v.dependent == c.dependent &&
                                v.value     == c.value;
                       }) == p.constraints.end ())
          {
            p.constraints.emplace_back (move (c));
          }
        }

        // Recursively collect our own dependents.
        //
        // Note that we cannot end up with an infinite recursion for
        // configured packages due to a dependency cycle since we "prune" for
        // visited dependencies (also see order() for details).
        //
        collect_dependents (dp, rpt_depts, unsatisfied_depts, visited_deps, r);
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
  print_constraints (diag_record& dr,
                     const build_package& p,
                     string& indent,
                     set<package_key>& printed,
                     optional<bool> existing_dependent) const
  {
    using constraint_type = build_package::constraint_type;

    const vector<constraint_type>& cs (p.constraints);

    if (!cs.empty ())
    {
      package_key pk (p.db, p.name ());

      if (printed.find (pk) == printed.end ())
      {
        printed.insert (pk);

        for (const constraint_type& c: cs)
        {
          if (!existing_dependent ||
              *existing_dependent == c.existing_dependent)
          {
            if (const build_package* d = dependent_build (c))
            {
              dr << '\n' << indent << c.dependent << " requires (" << pk
                 << ' ' << c.value << ')';

              indent += "  ";
              print_constraints (dr, *d, indent, printed, existing_dependent);
              indent.resize (indent.size () - 2);
            }
            else
              dr << '\n' << indent << c.dependent << " requires (" << pk << ' '
                 << c.value << ')';
          }
        }
      }
      else
      {
        for (const constraint_type& c: cs)
        {
          if (!existing_dependent ||
              *existing_dependent == c.existing_dependent)
          {
            dr << '\n' << indent << "...";
            break;
          }
        }
      }
    }
  }

  void build_packages::
  print_constraints (diag_record& dr,
                     const package_key& pk,
                     string& indent,
                     set<package_key>& printed,
                     optional<bool> existing_dependent) const
  {
    const build_package* p (entered_build (pk));
    assert (p != nullptr); // Expected to be collected.
    print_constraints (dr, *p, indent, printed, existing_dependent);
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
      if (bp.action.has_value () != (i != end ()))
      {
        diag_record dr (info);

        if (!bp.action)
        {
          dr << "pre-entered builds must never be ordered" <<
            info << "ordered pre-entered " << b.first;
        }
        else
        {
          dr << "build actions must be ordered" <<
            info << "unordered ";

          switch (*bp.action)
          {
          case build_package::build:
            {
              dr << "build " << bp.available_name_version_db () <<
                info << "flags 0x" << hex << uppercase << bp.flags;
              break;
            }
          case build_package::drop:
            {
              dr << "drop " << *bp.selected << bp.db;
              break;
            }
          case build_package::adjust:
            {
              dr << "adjustment " << *bp.selected << bp.db <<
                info << "flags 0x" << hex << uppercase << bp.flags;
              break;
            }
          }
        }

        dr << info
           << "please report in https://github.com/build2/build2/issues/318";

        dr.flush ();

        assert (bp.action.has_value () == (i != end ()));
      }
    }
  }

  vector<build_packages::existing_dependent> build_packages::
  query_existing_dependents (
    tracer& trace,
    const pkg_build_options& o,
    database& db,
    const package_name& name,
    bool exclude_optional,
    const function<find_database_function>& fdb,
    const repointed_dependents& rpt_depts,
    const replaced_versions& replaced_vers)
  {
    vector<existing_dependent> r;

    // Lazily search for the dependency build and detect if it is being
    // up/downgraded. Note that we will only do that if the dependency has an
    // existing dependent which imposes a version constraint on this
    // dependency.
    //
    const build_package* dep (nullptr);
    int ud (0);

    for (database& ddb: db.dependent_configs ())
    {
      for (auto& pd: query_dependents (ddb, name, db))
      {
        package_key pk (ddb, pd.name);

        // Ignore repointed dependents.
        //
        if (rpt_depts.find (pk) != rpt_depts.end ())
        {
          l5 ([&]{trace << "skip repointed existing dependent " << pk
                        << " of dependency " << name << db;});
          continue;
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
            l5 ([&]{trace << "skip being "
                          << (build ? "built" : "dropped")
                          << " existing dependent " << pk
                          << " of dependency " << name << db;});
            continue;
          }
        }

        // Ignore dependent if this dependency up/downgrade won't satisfy the
        // dependent's constraint. The thinking here is that we will either
        // fail for this reason later or the problem will be resolved
        // naturally due to the execution plan refinement (see
        // unsatisfied_dependents for details).
        //
        if (pd.constraint)
        {
          // Search for the dependency build and detect if it is being
          // up/downgraded, if not done yet. In particular, the available
          // package could be NULL meaning we are just adjusting.
          //
          if (dep == nullptr)
          {
            dep = entered_build (db, name);

            assert (dep != nullptr); // Expected to be being built.

            if (dep->available != nullptr)
            {
              const shared_ptr<selected_package>& sp (dep->selected);

              // Expected to be selected since it has an existing dependent.
              //
              assert (sp != nullptr);

              ud = sp->version.compare (dep->available_version ());
            }
          }

          if (ud != 0 &&
              !satisfies (dep->available_version (), *pd.constraint))
          {
            l5 ([&]{trace << "skip unsatisfied existing dependent " << pk
                          << " of dependency "
                          << dep->available_name_version_db () << " due to "
                          << "constraint (" << name << ' ' << *pd.constraint
                          << ')';});

            continue;
          }
        }

        // Pre-reevaluate the dependent to calculate the position which the
        // dependent should be re-evaluated to.
        //
        shared_ptr<selected_package> dsp (
          ddb.load<selected_package> (pd.name));

        pair<shared_ptr<available_package>,
             lazy_shared_ptr<repository_fragment>> rp (
               find_available_fragment (o, ddb, dsp));

        optional<package_key> orig_dep (package_key {db, name});

        try
        {
          build_package p {
            build_package::build,
            ddb,
            dsp, // Don't move from since will be used later.
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
            nullopt,                    // Upgrade.
            false,                      // Deorphan.
            {},                         // Required by (dependency).
            false,                      // Required by dependents.
            0};                         // State flags.

          build_package_refs dep_chain;
          postponed_packages postponed_repo;
          postponed_packages postponed_alts;
          postponed_packages postponed_recs;
          postponed_existing_dependencies postponed_edeps;
          postponed_dependencies postponed_deps;
          postponed_configurations postponed_cfgs;
          unacceptable_alternatives unacceptable_alts;
          unsatisfied_dependents unsatisfied_depts;
          replaced_versions replaced_vers;

          optional<pre_reevaluate_result> pr (
            collect_build_prerequisites (o,
                                         p,
                                         dep_chain,
                                         fdb,
                                         nullptr /* add_priv_cfg_function */,
                                         rpt_depts,
                                         replaced_vers,
                                         &postponed_repo,
                                         &postponed_alts,
                                         numeric_limits<size_t>::max (),
                                         postponed_recs,
                                         postponed_edeps,
                                         postponed_deps,
                                         postponed_cfgs,
                                         unacceptable_alts,
                                         unsatisfied_depts,
                                         pair<size_t, size_t> (0, 0),
                                         orig_dep));

          // Must be read-only.
          //
          assert (postponed_repo.empty ()    &&
                  postponed_alts.empty ()    &&
                  postponed_recs.empty ()    &&
                  postponed_edeps.empty ()   &&
                  postponed_deps.empty ()    &&
                  postponed_cfgs.empty ()    &&
                  unacceptable_alts.empty () &&
                  unsatisfied_depts.empty () &&
                  replaced_vers.empty ());

          if (pr && (!pr->reevaluation_optional || !exclude_optional))
          {
            // Try to preserve the name of the originating dependency as the
            // one which brings the existing dependent to the config cluster.
            // Failed that, use the first dependency in the alternative which
            // we will be re-evaluating to.
            //
            package_key dep (*orig_dep);

            pre_reevaluate_result::packages& deps (
              pr->reevaluation_dependencies);

            assert (!deps.empty ());

            if (find (deps.begin (), deps.end (), dep) == deps.end ())
              dep = move (deps.front ());

            r.push_back (
              existing_dependent {
                ddb, move (dsp),
                move (dep), pr->reevaluation_position,
                move (*orig_dep), pr->originating_dependency_position});
          }
        }
        catch (const reevaluation_deviated&)
        {
          r.push_back (
            existing_dependent {ddb, move (dsp),
                                nullopt, {},
                                move (*orig_dep), {}});
        }
      }
    }

    return r;
  }

  const build_package* build_packages::
  collect_existing_dependent_dependency (
    const pkg_build_options& o,
    const existing_dependent& ed,
    replaced_versions& replaced_vers,
    postponed_configurations& postponed_cfgs,
    unsatisfied_dependents& unsatisfied_depts)
  {
    assert (ed.dependency); // Shouldn't be called for deviated dependents.

    const shared_ptr<selected_package>& dsp (ed.selected);

    package_version_key dpt (ed.db, dsp->name, dsp->version);
    const package_key&  dep (*ed.dependency);

    lazy_shared_ptr<selected_package> lsp (dep.db.get (), dep.name);
    shared_ptr<selected_package>       sp (lsp.load ());

    pair<shared_ptr<available_package>,
         lazy_shared_ptr<repository_fragment>> rp (
           find_available_fragment (o, dep.db, sp));

    bool system (sp->system ());

    build_package p {
      build_package::build,
      dep.db,
      move (sp),
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
      nullopt,                    // Upgrade.
      false,                      // Deorphan.
      {dpt},                      // Required by (dependent).
      true,                       // Required by dependents.
      0};                         // State flags.

    // Add constraints, if present.
    //
    {
      auto i (dsp->prerequisites.find (lsp));
      assert (i != dsp->prerequisites.end ());

      if (i->second.constraint)
        p.constraints.emplace_back (*i->second.constraint,
                                    dpt.db,
                                    dpt.name,
                                    *dpt.version,
                                    true /* existing_package */);
    }

    // Note: not recursive.
    //
    collect_build (
      o, move (p), replaced_vers, postponed_cfgs, unsatisfied_depts);

    return entered_build (dep);
  }

  void build_packages::
  collect_existing_dependent (const pkg_build_options& o,
                              const existing_dependent& ed,
                              postponed_configuration::packages&& ds,
                              replaced_versions& replaced_vers,
                              postponed_configurations& postponed_cfgs,
                              unsatisfied_dependents& unsatisfied_depts)
  {
    assert (ed.dependency); // May not be a deviated existing dependent.

    pair<shared_ptr<available_package>,
         lazy_shared_ptr<repository_fragment>> rp (
           find_available_fragment (o, ed.db, ed.selected));

    set<package_version_key> rb;

    for (package_key& p: ds)
      rb.emplace (p.db, move (p.name), version ());

    build_package p {
      build_package::build,
      ed.db,
      ed.selected,
      move (rp.first),
      move (rp.second),
      nullopt,                            // Dependencies.
      nullopt,                            // Dependencies alternatives.
      nullopt,                            // Package skeleton.
      nullopt,                            // Postponed dependency alternatives.
      false,                              // Recursive collection.
      nullopt,                            // Hold package.
      nullopt,                            // Hold version.
      {},                                 // Constraints.
      false,                              // System.
      false,                              // Keep output directory.
      false,                              // Disfigure (from-scratch reconf).
      false,                              // Configure-only.
      nullopt,                            // Checkout root.
      false,                              // Checkout purge.
      strings (),                         // Configuration variables.
      nullopt,                            // Upgrade.
      false,                              // Deorphan.
      move (rb),                          // Required by (dependency).
      false,                              // Required by dependents.
      build_package::build_reevaluate};

    // Note: not recursive.
    //
    collect_build (
      o, move (p), replaced_vers, postponed_cfgs, unsatisfied_depts);
  }

  void build_packages::
  recollect_existing_dependent (const pkg_build_options& o,
                                const existing_dependent& ed,
                                replaced_versions& replaced_vers,
                                postponed_packages& postponed_recs,
                                postponed_configurations& postponed_cfgs,
                                unsatisfied_dependents& unsatisfied_depts,
                                bool add_required_by)
  {
    pair<shared_ptr<available_package>,
         lazy_shared_ptr<repository_fragment>> rp (
           find_available_fragment (o, ed.db, ed.selected));

    uint16_t flags (build_package::build_recollect);

    // Reconfigure the deviated dependents.
    //
    if (!ed.dependency)
      flags |= build_package::adjust_reconfigure;

    set<package_version_key> rb;

    if (add_required_by)
    {
      const package_key& pk (ed.originating_dependency);

      assert (entered_build (pk) != nullptr); // Expected to be collected.

      rb.emplace (pk.db, pk.name, version ());
    }

    build_package p {
      build_package::build,
      ed.db,
      ed.selected,
      move (rp.first),
      move (rp.second),
      nullopt,                     // Dependencies.
      nullopt,                     // Dependencies alternatives.
      nullopt,                     // Package skeleton.
      nullopt,                     // Postponed dependency alternatives.
      false,                       // Recursive collection.
      nullopt,                     // Hold package.
      nullopt,                     // Hold version.
      {},                          // Constraints.
      false,                       // System.
      false,                       // Keep output directory.
      false,                       // Disfigure (from-scratch reconf).
      false,                       // Configure-only.
      nullopt,                     // Checkout root.
      false,                       // Checkout purge.
      strings (),                  // Configuration variables.
      nullopt,                     // Upgrade.
      false,                       // Deorphan.
      move (rb),                   // Required by (dependency).
      false,                       // Required by dependents.
      flags};

    // Note: not recursive.
    //
    collect_build (
      o, move (p), replaced_vers, postponed_cfgs, unsatisfied_depts);

    postponed_recs.insert (entered_build (ed.db, ed.selected->name));
  }

  build_packages::iterator build_packages::
  order (database& db,
         const package_name& name,
         package_refs& chain,
         const function<find_database_function>& fdb,
         bool reorder)
  {
    package_map::iterator mi (map_.find (db, name));

    // Every package that we order should have already been collected.
    //
    assert (mi != map_.end ());

    build_package& p (mi->second.package);

    assert (p.action); // Can't order just a pre-entered package.

    // Make sure there is no dependency cycle.
    //
    package_ref cp {db, name};
    {
      auto i (find (chain.begin (), chain.end (), cp));

      if (i != chain.end ())
      {
        diag_record dr (fail);
        dr << "dependency cycle detected involving package " << name << db;

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

    // Similar to collect_build_prerequisites(), we can prune if the package
    // is already configured, right? While in collect_build_prerequisites() we
    // didn't need to add prerequisites of such a package, it doesn't mean
    // that they actually never ended up in the map via another dependency
    // path. For example, some can be a part of the initial selection. And in
    // that case we must order things properly.
    //
    // Also, if the package we are ordering is not a system one and needs to
    // be disfigured during the plan execution, then we must order its
    // (current) dependencies that also need to be disfigured.
    //
    // And yet, if the package we are ordering is a repointed dependent, then
    // we must order not only its unamended and new prerequisites
    // (prerequisite replacements) but also its replaced prerequisites, which
    // can also be disfigured.
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
      // the package prerequisites builds are collected or not. If they are
      // not, then the package is being reconfigured and we use its configured
      // prerequisites list. Otherwise, we use its collected prerequisites
      // builds.
      //
      if (!p.dependencies)
      {
        assert (src_conf); // Shouldn't be here otherwise.

        // A repointed dependent have always its prerequisite replacements
        // collected, so p.dependencies must always be present for them.
        //
        assert ((p.flags & build_package::build_repoint) == 0);

        for (const auto& p: sp->prerequisites)
        {
          database& db (p.first.database ());
          const package_name& name (p.first.object_id ());

          // The prerequisites may not necessarily be in the map or have an
          // action be present, but they can never be dropped.
          //
          auto i (map_.find (db, name));
          if (i != map_.end ())
          {
            optional<build_package::action_type> a (i->second.package.action);

            assert (!a || *a != build_package::drop); // See above.

            if (a)
              update (order (db, name, chain, fdb, false /* reorder */));
          }
        }

        // We just ordered them among other prerequisites.
        //
        order_disfigured = false;
      }
      else
      {
        // If the package prerequisites builds are collected, then the
        // resulting dependency list must be complete.
        //
        assert (p.dependencies->size () == ap->dependencies.size ());

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

          bool buildtime (das.buildtime);

          for (const dependency& d: das.front ())
          {
            const package_name& n (d.name);

            // Use the custom search function to find the dependency's build
            // configuration. Failed that, search for it recursively.
            //
            database* ddb (fdb (db, n, buildtime));

            auto i (ddb != nullptr
                    ? map_.find (*ddb, n)
                    : map_.find_dependency (db, n, buildtime));

            // Note that for the repointed dependent we only order its new and
            // potentially unamended prerequisites here (see
            // collect_build_prerequisites() for details). Thus its
            // (unamended) prerequisites may not necessarily be in the map or
            // have an action be present, but they can never be dropped. Its
            // replaced prerequisites will be ordered below.
            //
            if (i != map_.end ())
            {
              optional<build_package::action_type> a (
                i->second.package.action);

              assert (!a || *a != build_package::drop); // See above.

              if (a)
              {
                update (order (i->first.db,
                               n,
                               chain,
                               fdb,
                               false /* reorder */));
              }
            }
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
          update (order (db, name, chain, fdb, false /* reorder */));
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
