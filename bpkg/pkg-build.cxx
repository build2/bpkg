// file      : bpkg/pkg-build.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-build.hxx>

#include <map>
#include <set>
#include <cstring>  // strlen()
#include <sstream>
#include <iostream> // cout

#include <libbutl/standard-version.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/satisfaction.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/common-options.hxx>

#include <bpkg/cfg-link.hxx>
#include <bpkg/rep-mask.hxx>
#include <bpkg/pkg-purge.hxx>
#include <bpkg/pkg-fetch.hxx>
#include <bpkg/rep-fetch.hxx>
#include <bpkg/pkg-unpack.hxx>
#include <bpkg/pkg-update.hxx>
#include <bpkg/pkg-verify.hxx>
#include <bpkg/pkg-checkout.hxx>
#include <bpkg/pkg-configure.hxx>
#include <bpkg/pkg-disfigure.hxx>
#include <bpkg/package-query.hxx>
#include <bpkg/package-skeleton.hxx>

#include <bpkg/system-repository.hxx>
#include <bpkg/system-package-manager.hxx>

#include <bpkg/pkg-build-collect.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  // System package manager. Resolved lazily if and when needed. Present NULL
  // value means no system package manager is available for this host.
  //
  static optional<unique_ptr<system_package_manager>> sys_pkg_mgr;

  // Current configurations as specified with --directory|-d (or the current
  // working directory if none specified).
  //
  static linked_databases current_configs;

  static inline bool
  multi_config ()
  {
    return current_configs.size () != 1;
  }

  static inline bool
  current (database& db)
  {
    return find (current_configs.begin (), current_configs.end (), db) !=
           current_configs.end ();
  }

  // Retrieve the repository fragments for the specified package from its
  // ultimate dependent configurations and add them to the respective
  // configuration-associated fragment lists.
  //
  // If this package's repository fragment is a root fragment (package is
  // fetched/unpacked using the existing archive/directory), then also add
  // this repository fragment to the resulting list assuming that this
  // package's dependencies can be resolved from this repository fragment or
  // its complements (user-added repositories) as well.
  //
  static void
  add_dependent_repo_fragments (database& db,
                                const shared_ptr<selected_package>& p,
                                config_repo_fragments& r)
  {
    available_package_id id (p->name, p->version);

    // Add a repository fragment to the specified list, suppressing duplicates.
    //
    auto add = [] (shared_ptr<repository_fragment>&& rf,
                   vector<shared_ptr<repository_fragment>>& rfs)
    {
      if (find (rfs.begin (), rfs.end (), rf) == rfs.end ())
        rfs.push_back (move (rf));
    };

    if (p->repository_fragment.empty ()) // Root repository fragment?
      add (db.find<repository_fragment> (empty_string), r[db]);

    for (database& ddb: dependent_repo_configs (db))
    {
      shared_ptr<available_package> dap (ddb.find<available_package> (id));

      if (dap != nullptr)
      {
        assert (!dap->locations.empty ());

        config_repo_fragments::iterator i (r.find (ddb));

        if (i == r.end ())
          i = r.insert (ddb,
                        vector<shared_ptr<repository_fragment>> ()).first;

        vector<shared_ptr<repository_fragment>>& rfs (i->second);

        for (const auto& pl: dap->locations)
        {
          const lazy_shared_ptr<repository_fragment>& lrf (
            pl.repository_fragment);

          if (!rep_masked_fragment (lrf))
            add (lrf.load (), rfs);
        }

        // Erase the entry from the map if it contains no fragments, which may
        // happen if all the available package repositories are masked.
        //
        if (rfs.empty ())
          r.erase (i);
      }
    }
  }

  // Return a patch version constraint for the specified package version if it
  // is a standard version (~ shortcut). Otherwise, if requested, issue a
  // warning and return nullopt.
  //
  // Note that the function may also issue a warning and return nullopt if the
  // package minor version reached the limit (see standard-version.cxx for
  // details).
  //
  static optional<version_constraint>
  patch_constraint (const package_name& nm,
                    const version& pv,
                    bool quiet = false)
  {
    // Note that we don't pass allow_stub flag so the system wildcard version
    // will (naturally) not be patched.
    //
    string vs (pv.string ());
    optional<standard_version> v (parse_standard_version (vs));

    if (!v)
    {
      if (!quiet)
        warn << "unable to patch " << package_string (nm, pv) <<
          info << "package is not using semantic/standard version";

      return nullopt;
    }

    try
    {
      return version_constraint ('~' + vs);
    }
    // Note that the only possible reason for invalid_argument exception to be
    // thrown is that minor version reached the 99999 limit (see
    // standard-version.cxx for details).
    //
    catch (const invalid_argument&)
    {
      if (!quiet)
        warn << "unable to patch " << package_string (nm, pv) <<
          info << "minor version limit reached";

      return nullopt;
    }
  }

  static inline optional<version_constraint>
  patch_constraint (const shared_ptr<selected_package>& sp, bool quiet = false)
  {
    return patch_constraint (sp->name, sp->version, quiet);
  }

  // As above but returns a minor version constraint (^ shortcut) instead of
  // the patch version constraint (~ shortcut).
  //
  static optional<version_constraint>
  minor_constraint (const package_name& nm,
                    const version& pv,
                    bool quiet = false)
  {
    // Note that we don't pass allow_stub flag so the system wildcard version
    // will (naturally) not be patched.
    //
    string vs (pv.string ());
    optional<standard_version> v (parse_standard_version (vs));

    if (!v)
    {
      if (!quiet)
        warn << "unable to upgrade " << package_string (nm, pv)
             << " to latest minor version" <<
          info << "package is not using semantic/standard version";

      return nullopt;
    }

    try
    {
      return version_constraint ('^' + vs);
    }
    // Note that the only possible reason for invalid_argument exception to be
    // thrown is that major version reached the 99999 limit (see
    // standard-version.cxx for details).
    //
    catch (const invalid_argument&)
    {
      if (!quiet)
        warn << "unable to upgrade " << package_string (nm, pv)
             << " to latest minor version" <<
          info << "major version limit reached";

      return nullopt;
    }
  }

  // Return true if the selected package is not configured as system and its
  // repository fragment is not present in any ultimate dependent
  // configuration (see dependent_repo_configs() for details) or this exact
  // version is not available from this repository fragment nor from its
  // complements. Also return true if the selected package repository fragment
  // is a root fragment (package is fetched/unpacked using the existing
  // archive/directory).
  //
  // Note that the orphan definition here is stronger than in the rest of the
  // code, since we request the available package to also be present in the
  // repository fragment and consider packages built as existing archives or
  // directories as orphans. It feels that such a definition aligns better
  // with the user expectations about deorphaning.
  //
  static bool
  orphan_package (database& db, const shared_ptr<selected_package>& sp)
  {
    assert (sp != nullptr);

    if (sp->system ())
      return false;

    const string& cn (sp->repository_fragment.canonical_name ());

    if (cn.empty ()) // Root repository fragment?
      return true;

    for (database& ddb: dependent_repo_configs (db))
    {
      const shared_ptr<repository_fragment> rf (
        ddb.find<repository_fragment> (cn));

      if (rf != nullptr && !rep_masked_fragment (ddb, rf))
      {
        auto af (
          find_available_one (sp->name,
                              version_constraint (sp->version),
                              lazy_shared_ptr<repository_fragment> (ddb,
                                                                    move (rf)),
                              false /* prereq */,
                              true /* revision */));

        const shared_ptr<available_package>& ap (af.first);

        if (ap != nullptr && !ap->stub ())
          return false;
      }
    }

    return true;
  }

  // List of dependency packages (specified with ? on the command line).
  //
  // If configuration is not specified for a system dependency package (db is
  // NULL), then the dependency is assumed to be specified for all current
  // configurations and their explicitly linked configurations, recursively,
  // including private configurations that can potentially be created during
  // this run.
  //
  // The selected package is not NULL if the database is not NULL and the
  // dependency package is present in this database.
  //
  struct dependency_package
  {
    database*                    db;            // Can only be NULL if system.
    package_name                 name;
    optional<version_constraint> constraint;    // nullopt if unspecified.

    // Can only be true if constraint is specified.
    //
    bool                         hold_version;

    shared_ptr<selected_package> selected;
    bool                         system;
    bool                         existing;      // Build as archive or directory.

    // true -- upgrade, false -- patch.
    //
    optional<bool>               upgrade;       // Only for absent constraint.

    bool                         deorphan;
    bool                         keep_out;
    bool                         disfigure;
    optional<dir_path>           checkout_root;
    bool                         checkout_purge;
    strings                      config_vars;   // Only if not system.
    const system_package_status* system_status; // See struct pkg_arg.
  };
  using dependency_packages = vector<dependency_package>;

  // Evaluate a dependency package and return a new desired version. If the
  // result is absent (nullopt), then there are no user expectations regarding
  // this dependency. If the result is a NULL available_package, then it is
  // either no longer used and can be dropped, or no changes to the dependency
  // are necessary. Otherwise, the result is available_package to
  // upgrade/downgrade to or replace with the same version (deorphan, rebuild
  // as an existing archive or directory, etc) as well as the repository
  // fragment it must come from, the system flag, and the database it must be
  // configured in.
  //
  // If the dependency is being rebuilt as an existing archive or directory we
  // may end up with the available package version being the same as the
  // selected package version. In this case the dependency needs to be
  // re-fetched/re-unpacked from this archive/directory. Also note that if the
  // dependency needs to be rebuilt as an existing archive or directory the
  // caller may need to stash its name/database. This way on the subsequent
  // call this function may return the "no change" recommendation rather than
  // the "replace" recommendation.
  //
  // If in the deorphan mode it turns out that the package is not an orphan
  // and there is no version constraint specified and upgrade/patch is not
  // requested, then assume that no changes are necessary for the dependency.
  // Otherwise, if the package version is not constrained and no upgrade/patch
  // is requested, then pick the version that matches the dependency version
  // best in the following preference order:
  //
  // - same version, revision, and iteration
  // - latest iteration of same version and revision
  // - later revision of same version
  // - later patch of same version
  // - later minor of same version
  // - latest available version, including earlier
  //
  // Otherwise, always upgrade/downgrade the orphan or fail if no satisfactory
  // version is available. Note that in the both cases (deorphan and
  // upgrade/downgrade+deorphan) we may end up with the available package
  // version being the same as the selected package version. In this case the
  // dependency needs to be re-fetched from an existing repository. Also note
  // that if the dependency needs to be deorphaned the caller may need to
  // cache the original orphan version. This way on the subsequent calls this
  // function still considers this package as an orphan and uses its original
  // version to deduce the best match, which may change due, for example, to a
  // change of the constraining dependents set.
  //
  // If the package version that satisfies explicitly specified dependency
  // version constraint can not be found in the dependents repositories, then
  // return the "no changes are necessary" result if ignore_unsatisfiable
  // argument is true and fail otherwise. The common approach is to pass true
  // for this argument until the execution plan is finalized, assuming that
  // the problematic dependency might be dropped.
  //
  struct evaluate_result
  {
    // The system, existing, upgrade, and orphan members are meaningless if
    // the unused flag is true.
    //
    reference_wrapper<database>                db;
    shared_ptr<available_package>              available;
    lazy_shared_ptr<bpkg::repository_fragment> repository_fragment;
    bool                                       unused;
    bool                                       system;
    bool                                       existing;
    optional<bool>                             upgrade;

    // Original orphan version which needs to be deorphaned. May only be
    // present for the deorphan mode.
    //
    optional<version>                          orphan;
  };

  struct dependent_constraint
  {
    database&                    db;
    shared_ptr<selected_package> package;
    optional<version_constraint> constraint;

    dependent_constraint (database& d,
                          shared_ptr<selected_package> p,
                          optional<version_constraint> c)
        : db (d), package (move (p)), constraint (move (c)) {}
  };

  using dependent_constraints = vector<dependent_constraint>;
  using deorphaned_dependencies = map<package_key, version>;
  using existing_dependencies = vector<package_key>;

  static evaluate_result
  evaluate_dependency (const common_options&,
                       database&,
                       const shared_ptr<selected_package>&,
                       const optional<version_constraint>& desired,
                       bool desired_sys,
                       bool existing,
                       database& desired_db,
                       const shared_ptr<selected_package>& desired_db_sp,
                       optional<bool> upgrade,
                       bool deorphan,
                       bool explicitly,
                       const config_repo_fragments&,
                       const dependent_constraints&,
                       const existing_dependencies&,
                       const deorphaned_dependencies&,
                       const build_packages&,
                       bool ignore_unsatisfiable);

  // If there are no user expectations regarding this dependency, then we give
  // no up/down-grade/replace recommendation, unless there are no dependents
  // in which case we recommend to drop the dependency.
  //
  // Note that the user expectations are only applied for dependencies that
  // have dependents in the current configurations.
  //
  static optional<evaluate_result>
  evaluate_dependency (const common_options& o,
                       database& db,
                       const shared_ptr<selected_package>& sp,
                       const dependency_packages& deps,
                       bool no_move,
                       const existing_dependencies& existing_deps,
                       const deorphaned_dependencies& deorphaned_deps,
                       const build_packages& pkgs,
                       bool ignore_unsatisfiable)
  {
    tracer trace ("evaluate_dependency");

    assert (sp != nullptr && !sp->hold_package);

    const package_name& nm (sp->name);

    auto no_change = [&db] ()
    {
      return evaluate_result {db,
                              nullptr /* available */,
                              nullptr /* repository_fragment */,
                              false   /* unused */,
                              false   /* system */,
                              false   /* existing */,
                              nullopt /* upgrade */,
                              nullopt /* orphan */};
    };

    // Only search for the user expectations regarding this dependency if it
    // has dependents in the current configurations, unless --no-move is
    // specified.
    //
    // In the no-move mode consider the user-specified configurations not as a
    // dependency new location, but as the current location of the dependency
    // to which the expectations are applied. Note that multiple package specs
    // for the same dependency in different configurations can be specified on
    // the command line.
    //
    linked_databases cur_dbs;
    dependency_packages::const_iterator i (deps.end ());

    if (!no_move)
    {
      // Collect the current configurations which contain dependents for this
      // dependency and assume no expectations if there is none.
      //
      for (database& cdb: current_configs)
      {
        if (!query_dependents (cdb, nm, db).empty ())
          cur_dbs.push_back (cdb);
      }

      // Search for the user expectations regarding this dependency by
      // matching the package name and configuration type, if configuration is
      // specified, preferring entries with configuration specified and fail
      // if there are multiple candidates.
      //
      if (!cur_dbs.empty ())
      {
        for (dependency_packages::const_iterator j (deps.begin ());
             j != deps.end ();
             ++j)
        {
          if (j->name == nm && (j->db == nullptr || j->db->type == db.type))
          {
            if (i == deps.end () || i->db == nullptr)
            {
              i = j;
            }
            else if (j->db != nullptr)
            {
              fail << "multiple " << db.type << " configurations specified "
                   << "for dependency package " << nm <<
                info << i->db->config_orig <<
                info << j->db->config_orig <<
                info << "consider using the --no-move option";
            }
          }
        }
      }
    }
    else
    {
      for (dependency_packages::const_iterator j (deps.begin ());
           j != deps.end ();
           ++j)
      {
        if (j->name == nm && (j->db == nullptr || *j->db == db))
        {
          if (i == deps.end () || i->db == nullptr)
            i = j;

          if (i->db != nullptr)
            break;
        }
      }
    }

    bool user_exp (i != deps.end ());
    bool copy_dep (user_exp && i->db != nullptr && *i->db != db);

    // Collect the dependents for checking the version constraints, using
    // their repository fragments for discovering available dependency package
    // versions, etc.
    //
    // Note that if dependency needs to be copied, then we only consider its
    // dependents in the current configurations which potentially can be
    // repointed to it. Note that configurations of such dependents must
    // contain the new dependency configuration in their dependency tree.
    //
    linked_databases dep_dbs;

    if (copy_dep)
    {
      for (database& db: i->db->dependent_configs ())
      {
        if (find (cur_dbs.begin (), cur_dbs.end (), db) != cur_dbs.end ())
          dep_dbs.push_back (db);
      }

      // Bail out if no dependents can be repointed to the dependency.
      //
      if (dep_dbs.empty ())
      {
        l5 ([&]{trace << *sp << db << ": can't repoint";});
        return no_change ();
      }
    }
    else
      dep_dbs = db.dependent_configs ();

    // Collect the dependents but bail out if the dependency is used but there
    // are no user expectations regarding it.
    //
    vector<pair<database&, package_dependent>> pds;

    for (database& ddb: dep_dbs)
    {
      auto ds (query_dependents (ddb, nm, db));

      if (!ds.empty ())
      {
        if (!user_exp)
          return nullopt;

        for (auto& d: ds)
          pds.emplace_back (ddb, move (d));
      }
    }

    // Bail out if the dependency is unused.
    //
    if (pds.empty ())
    {
      l5 ([&]{trace << *sp << db << ": unused";});

      return evaluate_result {db,
                              nullptr /* available */,
                              nullptr /* repository_fragment */,
                              true    /* unused */,
                              false   /* system */,
                              false   /* existing */,
                              nullopt /* upgrade */,
                              nullopt /* orphan */};
    }

    // The requested dependency database, version constraint, and system flag.
    //
    assert (i != deps.end ());

    database& ddb (i->db != nullptr ? *i->db : db);
    const optional<version_constraint>& dvc (i->constraint); // May be nullopt.
    bool dsys (i->system);
    bool existing (i->existing);
    bool deorphan (i->deorphan);

    // The selected package in the desired database which we copy over.
    //
    // It is the current dependency package, if we don't copy, and may or may
    // not exist otherwise.
    //
    shared_ptr<selected_package> dsp (db == ddb
                                      ? sp
                                      : ddb.find<selected_package> (nm));

    // If a package in the desired database is already selected and matches
    // the user expectations then no package change is required, unless the
    // package is also being built as an existing archive or directory or
    // needs to be deorphaned.
    //
    if (dsp != nullptr && dvc)
    {
      const version& sv (dsp->version);
      bool ssys (dsp->system ());

      if (!existing    &&
          !deorphan    &&
          ssys == dsys &&
          (ssys ? sv == *dvc->min_version : satisfies (sv, dvc)))
      {
        l5 ([&]{trace << *dsp << ddb << ": unchanged";});
        return no_change ();
      }
    }

    // Build a set of repository fragments the dependent packages come from.
    // Also cache the dependents and the constraints they apply to this
    // dependency.
    //
    config_repo_fragments repo_frags;
    dependent_constraints dpt_constrs;

    for (auto& pd: pds)
    {
      database& ddb (pd.first);
      package_dependent& dep (pd.second);

      shared_ptr<selected_package> p (ddb.load<selected_package> (dep.name));
      add_dependent_repo_fragments (ddb, p, repo_frags);

      dpt_constrs.emplace_back (ddb, move (p), move (dep.constraint));
    }

    return evaluate_dependency (o,
                                db,
                                sp,
                                dvc,
                                dsys,
                                existing,
                                ddb,
                                dsp,
                                i->upgrade,
                                deorphan,
                                true /* explicitly */,
                                repo_frags,
                                dpt_constrs,
                                existing_deps,
                                deorphaned_deps,
                                pkgs,
                                ignore_unsatisfiable);
  }

  struct config_selected_package
  {
    database& db;
    const shared_ptr<selected_package>& package;

    config_selected_package (database& d,
                             const shared_ptr<selected_package>& p)
        : db (d), package (p) {}

    bool
    operator== (const config_selected_package& v) const
    {
      return package->name == v.package->name && db == v.db;
    }

    bool
    operator< (const config_selected_package& v) const
    {
      int r (package->name.compare (v.package->name));
      return r != 0 ? (r < 0) : (db < v.db);
    }
  };

  static evaluate_result
  evaluate_dependency (const common_options& o,
                       database& db,
                       const shared_ptr<selected_package>& sp,
                       const optional<version_constraint>& dvc,
                       bool dsys,
                       bool existing,
                       database& ddb,
                       const shared_ptr<selected_package>& dsp,
                       optional<bool> upgrade,
                       bool deorphan,
                       bool explicitly,
                       const config_repo_fragments& rfs,
                       const dependent_constraints& dpt_constrs,
                       const existing_dependencies& existing_deps,
                       const deorphaned_dependencies& deorphaned_deps,
                       const build_packages& pkgs,
                       bool ignore_unsatisfiable)
  {
    tracer trace ("evaluate_dependency");

    const package_name& nm (sp->name);

    auto no_change = [&db] ()
    {
      return evaluate_result {db,
                              nullptr /* available */,
                              nullptr /* repository_fragment */,
                              false   /* unused */,
                              false   /* system */,
                              false   /* existing */,
                              nullopt /* upgrade */,
                              nullopt /* orphan */};
    };

    // Build the list of available packages for the potential up/down-grade
    // to, in the version-descending order. If patching, then we constrain the
    // choice with the latest patch version and place no constraints if
    // upgrading. For a system package we will try to find the available
    // package that matches the user-specified system version (preferable for
    // the configuration negotiation machinery) and, if fail, fallback to
    // picking the latest one just to make sure the package is recognized.
    //
    // But first check if this package is specified as an existing archive or
    // directory. If that's the case, then only consider its (transient)
    // available package instead of the above.
    //
    bool patch (false);
    available_packages afs;

    if (existing)
    {
      // By definition such a dependency has a version specified and may not
      // be system.
      //
      assert (dvc && !dsys);

      pair<shared_ptr<available_package>,
           lazy_shared_ptr<repository_fragment>> rp (
             find_existing (ddb, nm, *dvc));

      // Must have been added to the existing packages registry.
      //
      assert (rp.first != nullptr);

      afs.push_back (move (rp));
    }
    else
    {
      optional<version_constraint> c;

      if (!dvc)
      {
        assert (!dsys); // The version can't be empty for the system package.

        patch = (upgrade && !*upgrade);

        if (patch)
        {
          c = patch_constraint (sp, ignore_unsatisfiable);

          if (!c)
          {
            l5 ([&]{trace << *sp << db << ": non-patchable";});
            return no_change ();
          }
        }
      }
      else if (!dsys || !wildcard (*dvc))
        c = dvc;

      afs = find_available (nm, c, rfs);

      if (afs.empty () && dsys && c)
        afs = find_available (nm, nullopt, rfs);
    }

    // In the deorphan mode check that the dependency is an orphan or was
    // deorphaned on some previous refinement iteration. If that's not the
    // case, then just disable the deorphan mode for this dependency and, if
    // the version is not constrained and upgrade/patch is not requested, bail
    // out indicating that no change is required.
    //
    // Note that in the move mode (dsp != sp) we deorphan the dependency in
    // its destination configuration, if present. In the worst case scenario
    // both the source and destination selected packages may need to be
    // deorphaned since the source selected package may also stay if some
    // dependents were not repointed to the new dependency (remember that the
    // move mode is actually a copy mode). We, however, have no easy way to
    // issue recommendations for both the old and the new dependencies at the
    // moment. Given that in the common case the old dependency get dropped,
    // let's keep it simple and do nothing about the old dependency and see
    // how it goes.
    //
    const version* deorphaned (nullptr);

    if (deorphan)
    {
      bool orphan (dsp != nullptr && !dsp->system () && !dsys);

      if (orphan)
      {
        auto i (deorphaned_deps.find (package_key (ddb, nm)));

        if (i == deorphaned_deps.end ())
          orphan = orphan_package (ddb, dsp);
        else
          deorphaned = &i->second;
      }

      if (!orphan)
      {
        if (!dvc && !upgrade)
        {
          l5 ([&]{trace << *sp << db << ": non-orphan";});
          return no_change ();
        }

        deorphan = false;
      }
    }

    // Go through up/down-grade candidates and pick the first one that
    // satisfies all the dependents. In the deorphan mode if the package
    // version is not constrained and upgrade/patch is not requested, then
    // pick the version that matches the dependency version best (see the
    // function description for details). Collect (and sort) unsatisfied
    // dependents per the unsatisfiable version in case we need to print them.
    //
    // NOTE: don't forget to update the find_orphan_match() lambda and the
    // try_replace_dependency() function if changing anything deorphan-related
    // here.
    //
    using sp_set = set<config_selected_package>;

    vector<pair<version, sp_set>> unsatisfiable;

    bool stub (false);

    assert (!dsys ||
            (ddb.system_repository &&
             ddb.system_repository->find (nm) != nullptr));

    // Version to deorphan (original orphan version).
    //
    const version* dov (deorphaned != nullptr ? deorphaned    :
                        deorphan              ? &dsp->version :
                        nullptr);

    optional<version_constraint> dopc; // Patch constraint for the above.
    optional<version_constraint> domc; // Minor constraint for the above.

    bool orphan_best_match (deorphan && !dvc && !upgrade);

    if (orphan_best_match)
    {
      // Note that non-zero iteration makes a version non-standard, so we
      // reset it to 0 to produce the patch/minor constraints.
      //
      version v (dov->epoch,
                 dov->upstream,
                 dov->release,
                 dov->revision,
                 0 /* iteration */);

      dopc = patch_constraint (nm, v, true /* quiet */);
      domc = minor_constraint (nm, v, true /* quiet */);
    }

    using available = pair<shared_ptr<available_package>,
                           lazy_shared_ptr<repository_fragment>>;

    available deorphan_latest_iteration;
    available deorphan_later_revision;
    available deorphan_later_patch;
    available deorphan_later_minor;
    available deorphan_latest_available;

    // If the dependency is deorphaned to the same version as on the previous
    // call, then return the "no change" result. Otherwise, return the
    // deorphan result.
    //
    auto deorphan_result = [&sp, &db,
                            &ddb, &dsp,
                            dsys,
                            deorphaned, dov,
                            existing,
                            upgrade,
                            &no_change,
                            &trace] (available&& a, const char* what)
    {
      if (deorphaned != nullptr && dsp->version == a.first->version)
      {
        l5 ([&]{trace << *sp << db << ": already deorphaned";});
        return no_change ();
      }

      l5 ([&]{trace << *sp << db << ": deorphan to " << what << ' '
                    << package_string (sp->name, a.first->version)
                    << ddb;});

      return evaluate_result {
        ddb, move (a.first), move (a.second),
        false /* unused */,
        dsys,
        existing,
        upgrade,
        *dov};
    };

    auto build_result = [&ddb, dsys, existing, upgrade] (available&& a)
    {
      return evaluate_result {
        ddb, move (a.first), move (a.second),
        false /* unused */,
        dsys,
        existing,
        upgrade,
        nullopt /* orphan */};
    };

    // Note that if the selected dependency is the best that we can get, we
    // normally issue the "no change" recommendation. However, if the
    // configuration variables are specified for this dependency on the
    // command line, then we issue the "reconfigure" recommendation instead.
    //
    // Return true, if the already selected dependency has been specified on
    // the command line with the configuration variables, but has not yet been
    // built on this pkg-build run.
    //
    auto reconfigure = [&ddb, &dsp, &nm, dsys, &pkgs] ()
    {
      assert (dsp != nullptr);

      if (!dsys)
      {
        const build_package* p (pkgs.entered_build (ddb, nm));
        return p != nullptr && !p->action && !p->config_vars.empty ();
      }
      else
        return false;
    };

    for (available& af: afs)
    {
      shared_ptr<available_package>& ap (af.first);
      const version& av (!dsys ? ap->version : *ap->system_version (ddb));

      // If we aim to upgrade to the latest version and it tends to be less
      // then the selected one, then what we currently have is the best that
      // we can get, and so we return the "no change" result, unless we are
      // deorphaning.
      //
      // Note that we also handle a package stub here.
      //
      if (!dvc && dsp != nullptr && av < dsp->version)
      {
        assert (!dsys); // Version can't be empty for the system package.

        // For the selected system package we still need to pick a source
        // package version to downgrade to.
        //
        if (!dsp->system () && !deorphan)
        {
          if (reconfigure ())
          {
            l5 ([&]{trace << *dsp << ddb << ": reconfigure (best)";});
            return build_result (find_available_fragment (o, ddb, dsp));
          }
          else
          {
            l5 ([&]{trace << *dsp << ddb << ": best";});
            return no_change ();
          }
        }

        // We can not upgrade the package to a stub version, so just skip it.
        //
        if (ap->stub ())
        {
          stub = true;
          continue;
        }
      }

      // Check if the version satisfies all the dependents and collect
      // unsatisfied ones.
      //
      bool satisfactory (true);
      sp_set unsatisfied_dependents;

      for (const auto& dp: dpt_constrs)
      {
        if (!satisfies (av, dp.constraint))
        {
          satisfactory = false;

          // Continue to collect dependents of the unsatisfiable version if
          // we need to print them before failing.
          //
          if (ignore_unsatisfiable)
            break;

          unsatisfied_dependents.emplace (dp.db, dp.package);
        }
      }

      if (!satisfactory)
      {
        if (!ignore_unsatisfiable)
          unsatisfiable.emplace_back (av, move (unsatisfied_dependents));

        // If the dependency is expected to be configured as system, then bail
        // out, as an available package version will always resolve to the
        // system one (see above).
        //
        if (dsys)
          break;

        continue;
      }

      if (orphan_best_match)
      {
        // If the exact orphan version is encountered, then we are done.
        //
        if (av == *dov)
          return deorphan_result (move (af), "exactly same version");

        // If the available package is of the same revision as orphan but a
        // different iteration, then save it as the latest iteration of same
        // orphan version and revision.
        //
        if (deorphan_latest_iteration.first == nullptr &&
            av.compare (*dov, false /* revision */, true /* iteration */) == 0)
          deorphan_latest_iteration = af;

        // If the available package is of the same version as orphan and its
        // revision is greater, then save it as the later revision of same
        // version.
        //
        if (deorphan_later_revision.first == nullptr    &&
            av.compare (*dov, true /* revision */) == 0 &&
            av.compare (*dov, false /* revision */, true /* iteration */) > 0)
          deorphan_later_revision = af;

        // If the available package is of the same minor version as orphan but
        // of the greater patch version, then save it as the later patch of
        // same version.
        //
        if (deorphan_later_patch.first == nullptr &&
            dopc && satisfies (av, *dopc)         &&
            av.compare (*dov, true /* revision */) > 0) // Patch is greater?
          deorphan_later_patch = af;

        // If the available package is of the same major version as orphan but
        // of the greater minor version, then save it as the later minor of
        // same version.
        //
        // Note that we can't easily compare minor versions here since these
        // are bpkg version objects. Thus, we consider that this is a greater
        // minor version if the version is greater (ignoring revisions) and
        // the latest patch is not yet saved.
        //
        if (deorphan_later_minor.first == nullptr      &&
            domc && satisfies (av, *domc)              &&
            av.compare (*dov, true /* revision */) > 0 &&
            deorphan_later_patch.first == nullptr)
          deorphan_later_minor = af;

        // Save the latest available package version.
        //
        if (deorphan_latest_available.first == nullptr)
          deorphan_latest_available = move (af);

        // If the available package version is less then the orphan revision
        // then we can bail out from the loop, since all the versions from the
        // preference list have already been encountered, if present.
        //
        if (av.compare (*dov, false /* revision */, true /* iteration */) < 0)
        {
          assert (deorphan_latest_iteration.first != nullptr ||
                  deorphan_later_revision.first != nullptr   ||
                  deorphan_later_patch.first != nullptr      ||
                  deorphan_later_minor.first != nullptr      ||
                  deorphan_latest_available.first != nullptr);
          break;
        }
      }
      else
      {
        // In the up/downgrade+deorphan mode always replace the dependency,
        // re-fetching it from an existing repository if the version stays the
        // same.
        //
        if (deorphan)
          return deorphan_result (move (af), "constrained version");

        // For the regular up/downgrade if the best satisfactory version and
        // the desired system flag perfectly match the ones of the selected
        // package, then no package change is required. Otherwise, recommend
        // an upgrade/downgrade/replacement.
        //
        // Note: we need to be careful here not to start yo-yo'ing for a
        // dependency being built as an existing archive or directory. For
        // such a dependency we need to return the "no change" recommendation
        // if any version recommendation (which may not change) has already
        // been returned.
        //
        if (dsp != nullptr         &&
            av == dsp->version     &&
            dsp->system () == dsys &&
            (!existing ||
             find (existing_deps.begin (), existing_deps.end (),
                   package_key (ddb, nm)) != existing_deps.end ()))
        {
          if (reconfigure ())
          {
            l5 ([&]{trace << *dsp << ddb << ": reconfigure";});
            return build_result (move (af));
          }
          else
          {
            l5 ([&]{trace << *dsp << ddb << ": unchanged";});
            return no_change ();
          }
        }
        else
        {
          l5 ([&]{trace << *sp << db << ": update to "
                        << package_string (nm, av, dsys) << ddb;});

          return build_result (move (af));
        }
      }
    }

    if (orphan_best_match)
    {
      if (deorphan_latest_iteration.first != nullptr)
        return deorphan_result (move (deorphan_latest_iteration),
                                "latest iteration");

      if (deorphan_later_revision.first != nullptr)
        return deorphan_result (move (deorphan_later_revision),
                                "later revision");

      if (deorphan_later_patch.first != nullptr)
        return deorphan_result (move (deorphan_later_patch), "later patch");

      if (deorphan_later_minor.first != nullptr)
        return deorphan_result (move (deorphan_later_minor), "later minor");

      if (deorphan_latest_available.first != nullptr)
        return deorphan_result (move (deorphan_latest_available),
                                "latest available");
    }

    // If we aim to upgrade to the latest version, then what we currently have
    // is the only thing that we can get, and so returning the "no change"
    // result, unless we need to upgrade a package configured as system or to
    // deorphan.
    //
    if (!dvc && dsp != nullptr && !dsp->system () && !deorphan)
    {
      assert (!dsys); // Version cannot be empty for the system package.

      if (reconfigure ())
      {
        l5 ([&]{trace << *dsp << ddb << ": reconfigure (only)";});
        return build_result (find_available_fragment (o, ddb, dsp));
      }
      else
      {
        l5 ([&]{trace << *dsp << ddb << ": only";});
        return no_change ();
      }
    }

    // If the version satisfying the desired dependency version constraint is
    // unavailable or unsatisfiable for some dependents then we fail, unless
    // requested not to do so. In the latter case we return the "no change"
    // result.
    //
    if (ignore_unsatisfiable)
    {
      l5 ([&]{trace << package_string (nm, dvc, dsys) << ddb
                    << (unsatisfiable.empty ()
                        ? ": no source"
                        : ": unsatisfiable");});

      return no_change ();
    }

    // If there are no unsatisfiable versions then the package is not present
    // (or is not available in source) in its dependents' repositories.
    //
    if (unsatisfiable.empty ())
    {
      diag_record dr (fail);

      if (patch)
      {
        // Otherwise, we should have bailed out earlier returning "no change"
        // (see above).
        //
        assert (dsp != nullptr && (dsp->system () || deorphan));

        // Patch (as any upgrade) of a system package is always explicit, so
        // we always fail and never treat the package as being up to date.
        //
        assert (explicitly);

        fail << "patch version for " << *sp << db << " is not available "
             << "from its dependents' repositories";
      }
      else if (!stub)
        fail << package_string (nm, dsys ? nullopt : dvc) << ddb
             << " is not available from its dependents' repositories";
      else // The only available package is a stub.
      {
        // Otherwise, we should have bailed out earlier, returning "no change"
        // rather then setting the stub flag to true (see above).
        //
        assert (!dvc && !dsys && dsp != nullptr && (dsp->system () || deorphan));

        fail << package_string (nm, dvc) << ddb << " is not available in "
             << "source from its dependents' repositories";
      }
    }

    // Issue the diagnostics and fail.
    //
    diag_record dr (fail);
    dr << "package " << nm << ddb << " doesn't satisfy its dependents";

    // Print the list of unsatisfiable versions together with dependents they
    // don't satisfy: up to three latest versions with no more than five
    // dependents each.
    //
    size_t nv (0);
    for (const auto& u: unsatisfiable)
    {
      dr << info << package_string (nm, u.first) << " doesn't satisfy";

      const sp_set& ps (u.second);

      size_t i (0), n (ps.size ());
      for (auto p (ps.begin ()); i != n; ++p)
      {
        // It would probably be nice to also print the unsatisfied constraint
        // here, but let's keep it simple for now.
        //
        dr << (i == 0 ? " " : ", ") << *p->package << p->db;

        if (++i == 5 && n != 6) // Printing 'and 1 more' looks stupid.
          break;
      }

      if (i != n)
        dr << " and " << n - i << " more";

      if (++nv == 3 && unsatisfiable.size () != 4)
        break;
    }

    if (nv != unsatisfiable.size ())
      dr << info << "and " << unsatisfiable.size () - nv << " more";

    dr << endf;
  }

  // List of dependent packages whose immediate/recursive dependencies must be
  // upgraded and/or deorphaned (specified with -i/-r on the command line).
  //
  struct recursive_package
  {
    database&      db;
    package_name   name;

    // Recursive/immediate upgrade/patch. Note the upgrade member is only
    // meaningful if recursive is present.
    //
    optional<bool> recursive; // true -- recursive, false -- immediate.
    bool           upgrade;   // true -- upgrade,   false -- patch.

    // Recursive/immediate deorphaning.
    //
    optional<bool> deorphan;  // true -- recursive, false -- immediate.
  };
  using recursive_packages = vector<recursive_package>;

  // Recursively check if immediate dependencies of this dependent must be
  // upgraded or patched and/or deorphaned.
  //
  // Cache the results of this function calls to avoid multiple traversals of
  // the same dependency graphs.
  //
  struct upgrade_dependencies_key
  {
    package_key dependent;
    bool recursion;

    bool
    operator< (const upgrade_dependencies_key& v) const
    {
      if (recursion != v.recursion)
        return recursion < v.recursion;

      return dependent < v.dependent;
    }
  };

  struct upgrade_deorphan
  {
    optional<bool> upgrade; // true -- upgrade, false -- patch.
    bool deorphan;
  };

  using upgrade_dependencies_cache = map<upgrade_dependencies_key,
                                         upgrade_deorphan>;

  static upgrade_deorphan
  upgrade_dependencies (database& db,
                        const package_name& nm,
                        const recursive_packages& rs,
                        upgrade_dependencies_cache& cache,
                        bool recursion = false)
  {
    // If the result of the upgrade_dependencies() call for these dependent
    // and recursion flag value is cached, then return that. Otherwise, cache
    // the calculated result prior to returning it to the caller.
    //
    upgrade_dependencies_key k {package_key (db, nm), recursion};
    {
      auto i (cache.find (k));

      if (i != cache.end ())
        return i->second;
    }

    auto i (find_if (rs.begin (), rs.end (),
                     [&nm, &db] (const recursive_package& i) -> bool
                     {
                       return i.name == nm && i.db == db;
                     }));

    upgrade_deorphan r {nullopt /* upgrade */, false /* deorphan */};

    if (i != rs.end ())
    {
      if (i->recursive && *i->recursive >= recursion)
        r.upgrade = i->upgrade;

      if (i->deorphan && *i->deorphan >= recursion)
        r.deorphan = true;

      // If we both upgrade and deorphan, then we can bail out since the value
      // may not change any further (upgrade wins over patch and deorphaning
      // can't be canceled).
      //
      if (r.upgrade && *r.upgrade && r.deorphan)
      {
        cache[move (k)] = r;
        return r;
      }
    }

    for (database& ddb: db.dependent_configs ())
    {
      for (auto& pd: query_dependents_cache (ddb, nm, db))
      {
        // Note that we cannot end up with an infinite recursion for
        // configured packages due to a dependency cycle (see order() for
        // details).
        //
        upgrade_deorphan ud (
          upgrade_dependencies (ddb, pd.name, rs, cache, true /* recursion */));

        if (ud.upgrade || ud.deorphan)
        {
          // Upgrade wins over patch.
          //
          if (ud.upgrade && (!r.upgrade || *r.upgrade < *ud.upgrade))
            r.upgrade = *ud.upgrade;

          if (ud.deorphan)
            r.deorphan = true;

          // If we both upgrade and deorphan, then we can bail out (see above
          // for details).
          //
          if (r.upgrade && *r.upgrade && r.deorphan)
          {
            cache[move (k)] = r;
            return r;
          }
        }
      }
    }

    cache[move (k)] = r;
    return r;
  }

  // Evaluate a package (not necessarily dependency) and return a new desired
  // version. If the result is absent (nullopt), then no changes to the
  // package are necessary. Otherwise, the result is available_package to
  // upgrade/downgrade to or replace with, as well as the repository fragment
  // it must come from.
  //
  // If the system package cannot be upgraded to the source one, not being
  // found in the dependents repositories, then return nullopt if
  // ignore_unsatisfiable argument is true and fail otherwise (see the
  // evaluate_dependency() function description for details).
  //
  static optional<evaluate_result>
  evaluate_recursive (const common_options& o,
                      database& db,
                      const shared_ptr<selected_package>& sp,
                      const recursive_packages& recs,
                      const existing_dependencies& existing_deps,
                      const deorphaned_dependencies& deorphaned_deps,
                      const build_packages& pkgs,
                      bool ignore_unsatisfiable,
                      upgrade_dependencies_cache& cache)
  {
    tracer trace ("evaluate_recursive");

    assert (sp != nullptr);

    // Build a set of repository fragment the dependent packages come from.
    // Also cache the dependents and the constraints they apply to this
    // dependency.
    //
    config_repo_fragments repo_frags;
    dependent_constraints dpt_constrs;

    // Only collect repository fragments (for best version selection) of
    // (immediate) dependents that have a hit (direct or indirect) in recs.
    // Note, however, that we collect constraints from all the dependents.
    //
    upgrade_deorphan ud {nullopt /* upgrade */, false /* deorphan */};

    for (database& ddb: db.dependent_configs ())
    {
      for (auto& pd: query_dependents_cache (ddb, sp->name, db))
      {
        shared_ptr<selected_package> p (ddb.load<selected_package> (pd.name));

        dpt_constrs.emplace_back (ddb, p, move (pd.constraint));

        upgrade_deorphan u (upgrade_dependencies (ddb, pd.name, recs, cache));

        if (u.upgrade || u.deorphan)
        {
          // Upgrade wins over patch.
          //
          if (u.upgrade && (!ud.upgrade || *ud.upgrade < *u.upgrade))
            ud.upgrade = *u.upgrade;

          if (u.deorphan)
            ud.deorphan = true;
        }
        else
          continue;

        // While we already know that the dependency upgrade is required, we
        // continue to iterate over dependents, collecting the repository
        // fragments and the constraints.
        //
        add_dependent_repo_fragments (ddb, p, repo_frags);
      }
    }

    if (!ud.upgrade && !ud.deorphan)
    {
      l5 ([&]{trace << *sp << db << ": no hit";});
      return nullopt;
    }

    pair<shared_ptr<available_package>,
         lazy_shared_ptr<repository_fragment>> rp (
           find_existing (db, sp->name, nullopt /* version_constraint */));

    optional<evaluate_result> r (
      evaluate_dependency (o,
                           db,
                           sp,
                           nullopt /* desired */,
                           false /* desired_sys */,
                           rp.first != nullptr /* existing */,
                           db,
                           sp,
                           ud.upgrade,
                           ud.deorphan,
                           false /* explicitly */,
                           repo_frags,
                           dpt_constrs,
                           existing_deps,
                           deorphaned_deps,
                           pkgs,
                           ignore_unsatisfiable));

    // Translate the "no change" result into nullopt.
    //
    assert (!r || !r->unused);
    return r && r->available == nullptr ? nullopt : r;
  }

  // Stack of the command line adjustments as per unsatisfied_dependents
  // description.
  //
  struct cmdline_adjustment
  {
    enum class adjustment_type: uint8_t
    {
      hold_existing, // Adjust constraint in existing build-to-hold spec.
      dep_existing,  // Adjust constraint in existing dependency spec.
      hold_new,      // Add new build-to-hold spec.
      dep_new        // Add new dependency spec.
    };

    adjustment_type                            type;
    reference_wrapper<database>                db;
    package_name                               name;
    bpkg::version                              version; // Replacement.

    // Meaningful only for the *_new types.
    //
    optional<bool>                             upgrade;
    bool                                       deorphan = false;

    // For the newly created or popped from the stack object the following
    // three members contain the package version replacement information.
    // Otherwise (pushed to the stack), they contain the original command line
    // spec information.
    //
    shared_ptr<available_package>              available; // NULL for dep_* types.
    lazy_shared_ptr<bpkg::repository_fragment> repository_fragment; // As above.
    optional<version_constraint>               constraint;

    // Create object of the hold_existing type.
    //
    cmdline_adjustment (database& d,
                        const package_name& n,
                        shared_ptr<available_package>&& a,
                        lazy_shared_ptr<bpkg::repository_fragment>&& f)
        : type (adjustment_type::hold_existing),
          db (d),
          name (n),
          version (a->version),
          available (move (a)),
          repository_fragment (move (f)),
          constraint (version_constraint (version)) {}

    // Create object of the dep_existing type.
    //
    cmdline_adjustment (database& d,
                        const package_name& n,
                        const bpkg::version& v)
        : type (adjustment_type::dep_existing),
          db (d),
          name (n),
          version (v),
          constraint (version_constraint (version)) {}

    // Create object of the hold_new type.
    //
    cmdline_adjustment (database& d,
                        const package_name& n,
                        shared_ptr<available_package>&& a,
                        lazy_shared_ptr<bpkg::repository_fragment>&& f,
                        optional<bool> u,
                        bool o)
        : type (adjustment_type::hold_new),
          db (d),
          name (n),
          version (a->version),
          upgrade (u),
          deorphan (o),
          available (move (a)),
          repository_fragment (move (f)),
          constraint (version_constraint (version)) {}

    // Create object of the dep_new type.
    //
    cmdline_adjustment (database& d,
                        const package_name& n,
                        const bpkg::version& v,
                        optional<bool> u,
                        bool o)
        : type (adjustment_type::dep_new),
          db (d),
          name (n),
          version (v),
          upgrade (u),
          deorphan (o),
          constraint (version_constraint (version)) {}
  };

  class cmdline_adjustments
  {
  public:
    cmdline_adjustments (vector<build_package>& hps, dependency_packages& dps)
        : hold_pkgs_ (hps),
          dep_pkgs_ (dps) {}

    // Apply the specified adjustment to the command line, push the adjustment
    // to the stack, and record the resulting command line state as the SHA256
    // checksum.
    //
    void
    push (cmdline_adjustment&& a)
    {
      using type = cmdline_adjustment::adjustment_type;

      // We always set the `== <version>` constraint in the resulting spec.
      //
      assert (a.constraint);

      database& db (a.db);
      const package_name& nm (a.name);
      package_version_key cmd_line (db.main_database (), "command line");

      switch (a.type)
      {
      case type::hold_existing:
        {
          auto i (find_hold_pkg (a));
          assert (i != hold_pkgs_.end ()); // As per adjustment type.

          build_package& bp (*i);
          swap (bp.available, a.available);
          swap (bp.repository_fragment, a.repository_fragment);

          if (!bp.constraints.empty ())
          {
            swap (bp.constraints[0].value, *a.constraint);
          }
          else
          {
            bp.constraints.emplace_back (move (*a.constraint),
                                         cmd_line.db,
                                         cmd_line.name.string ());
            a.constraint = nullopt;
          }

          break;
        }
      case type::dep_existing:
        {
          auto i (find_dep_pkg (a));
          assert (i != dep_pkgs_.end ()); // As per adjustment type.
          swap (i->constraint, a.constraint);
          break;
        }
      case type::hold_new:
        {
          // As per adjustment type.
          //
          assert (find_hold_pkg (a) == hold_pkgs_.end ());

          // Start the database transaction to perform the
          // database::find<selected_package> call, unless we are already in
          // the transaction.
          //
          transaction t (db, !transaction::has_current ());

          build_package bp {
            build_package::build,
            db,
            db.find<selected_package> (nm),
            move (a.available),
            move (a.repository_fragment),
            nullopt,                    // Dependencies.
            nullopt,                    // Dependencies alternatives.
            nullopt,                    // Package skeleton.
            nullopt,                    // Postponed dependency alternatives.
            false,                      // Recursive collection.
            true,                       // Hold package.
            false,                      // Hold version.
            {},                         // Constraints.
            false,                      // System.
            false,                      // Keep output directory.
            false,                      // Disfigure (from-scratch reconf).
            false,                      // Configure-only.
            nullopt,                    // Checkout root.
            false,                      // Checkout purge.
            strings (),                 // Configuration variables.
            a.upgrade,
            a.deorphan,
            {cmd_line},                 // Required by (command line).
            false,                      // Required by dependents.
            (a.deorphan
             ? build_package::build_replace
             : uint16_t (0))};

          t.commit ();

          bp.constraints.emplace_back (move (*a.constraint),
                                       cmd_line.db,
                                       cmd_line.name.string ());

          a.constraint = nullopt;

          hold_pkgs_.push_back (move (bp));
          break;
        }
      case type::dep_new:
        {
          // As per adjustment type.
          //
          assert (find_dep_pkg (a) == dep_pkgs_.end ());

          // Start the database transaction to perform the
          // database::find<selected_package> call, unless we are already in
          // the transaction.
          //
          transaction t (db, !transaction::has_current ());

          dep_pkgs_.push_back (
            dependency_package {&db,
                                nm,
                                move (*a.constraint),
                                false /* hold_version */,
                                db.find<selected_package> (nm),
                                false /* system */,
                                false /* existing */,
                                a.upgrade,
                                a.deorphan,
                                false /* keep_out */,
                                false /* disfigure */,
                                nullopt /* checkout_root */,
                                false /* checkout_purge */,
                                strings () /* config_vars */,
                                nullptr /* system_status */});

          t.commit ();

          a.constraint = nullopt;
          break;
        }
      }

      packages_.insert (package_version_key (db, nm, a.version));
      adjustments_.push_back (move (a));
      former_states_.insert (state ());
    }

    // Roll back the latest (default) or first command line adjustment, pop it
    // from the stack, and return the popped adjustment. Assume that the stack
    // is not empty.
    //
    // Note that the returned object can be pushed to the stack again.
    //
    cmdline_adjustment
    pop (bool first = false)
    {
      using type = cmdline_adjustment::adjustment_type;

      assert (!empty ());

      // Pop the adjustment.
      //
      cmdline_adjustment a (move (!first
                                  ? adjustments_.back ()
                                  : adjustments_.front ()));
      if (!first)
        adjustments_.pop_back ();
      else
        adjustments_.erase (adjustments_.begin ());

      packages_.erase (package_version_key (a.db, a.name, a.version));

      // Roll back the adjustment.
      //
      switch (a.type)
      {
      case type::hold_existing:
        {
          auto i (find_hold_pkg (a));
          assert (i != hold_pkgs_.end ());

          build_package& bp (*i);
          swap (bp.available, a.available);
          swap (bp.repository_fragment, a.repository_fragment);

          // Must contain the replacement version.
          //
          assert (!bp.constraints.empty ());

          version_constraint& c (bp.constraints[0].value);

          if (a.constraint) // Original spec contains a version constraint?
          {
            swap (c, *a.constraint);
          }
          else
          {
            a.constraint = move (c);
            bp.constraints.clear ();
          }

          break;
        }
      case type::dep_existing:
        {
          auto i (find_dep_pkg (a));
          assert (i != dep_pkgs_.end ());
          swap (i->constraint, a.constraint);
          break;
        }
      case type::hold_new:
        {
          auto i (find_hold_pkg (a));
          assert (i != hold_pkgs_.end ());

          build_package& bp (*i);
          a.available = move (bp.available);
          a.repository_fragment = move (bp.repository_fragment);

          // Must contain the replacement version.
          //
          assert (!bp.constraints.empty ());

          a.constraint = move (bp.constraints[0].value);

          hold_pkgs_.erase (i);
          break;
        }
      case type::dep_new:
        {
          auto i (find_dep_pkg (a));
          assert (i != dep_pkgs_.end ());

          a.constraint = move (i->constraint);

          dep_pkgs_.erase (i);
          break;
        }
      }

      return a;
    }

    // Return the specified adjustment's string representation in the
    // following form:
    //
    //  hold_existing: '<pkg>[ <constraint>][ <database>]' -> '<pkg> <constraint>'
    //  dep_existing:  '?<pkg>[ <constraint>][ <database>]' -> '?<pkg> <constraint>'
    //  hold_new:      '<pkg> <constraint>[ <database>]'
    //  dep_new:       '?<pkg> <constraint>[ <database>]'
    //
    // Note: the adjustment is assumed to be newly created or be popped from
    // the stack.
    //
    string
    to_string (const cmdline_adjustment& a) const
    {
      using type = cmdline_adjustment::adjustment_type;

      assert (a.constraint); // Since not pushed.

      const string& s (a.db.get ().string);

      switch (a.type)
      {
      case type::hold_existing:
        {
          string r ("'" + a.name.string ());

          auto i (find_hold_pkg (a));
          assert (i != hold_pkgs_.end ());

          const build_package& bp (*i);
          if (!bp.constraints.empty ())
            r += ' ' + bp.constraints[0].value.string ();

          if (!s.empty ())
            r += ' ' + s;

          r += "' -> '" + a.name.string () + ' ' + a.constraint->string () +
               "'";

          return r;
        }
      case type::dep_existing:
        {
          string r ("'?" + a.name.string ());

          auto i (find_dep_pkg (a));
          assert (i != dep_pkgs_.end ());

          if (i->constraint)
            r += ' ' + i->constraint->string ();

          if (!s.empty ())
            r += ' ' + s;

          r += "' -> '?" + a.name.string () + ' ' + a.constraint->string () +
               "'";

          return r;
        }
      case type::hold_new:
        {
          assert (find_hold_pkg (a) == hold_pkgs_.end ());

          string r ("'" + a.name.string () + ' ' + a.constraint->string ());

          if (!s.empty ())
            r += ' ' + s;

          r += "'";
          return r;
        }
      case type::dep_new:
        {
          assert (find_dep_pkg (a) == dep_pkgs_.end ());

          string r ("'?" + a.name.string () + ' ' + a.constraint->string ());

          if (!s.empty ())
            r += ' ' + s;

          r += "'";
          return r;
        }
      }

      assert (false); // Can't be here.
      return "";
    }

    // Return true, if there are no adjustments in the stack.
    //
    bool
    empty () const
    {
      return adjustments_.empty ();
    }

    // Return true, if push() has been called at least once.
    //
    bool
    tried () const
    {
      return !former_states_.empty ();
    }

    // Return the number of adjustments in the stack.
    //
    size_t
    size () const
    {
      return adjustments_.size ();
    }

    // Return true if replacing a package build with the specified version
    // will result in a command line which has already been (unsuccessfully)
    // tried as a starting point for the package builds re-collection.
    //
    bool
    tried_earlier (database& db, const package_name& n, const version& v) const
    {
      if (former_states_.empty ())
        return false;

      // Similar to the state() function, calculate the checksum over the
      // packages set, but also consider the specified package version as if
      // it were present in the set.
      //
      // Note that the specified package version may not be in the set, since
      // we shouldn't be trying to replace with the package version which is
      // already in the command line.
      //
      sha256 cs;

      auto lt = [&db, &n, &v] (const package_version_key& pvk)
      {
        if (int r = n.compare (pvk.name))
          return r < 0;

        if (int r = v.compare (*pvk.version))
          return r < 0;

        return db < pvk.db;
      };

      bool appended (false);
      for (const package_version_key& p: packages_)
      {
        assert (p.version); // Only the real packages can be here.

        if (!appended && lt (p))
        {
          cs.append (db.config.string ());
          cs.append (n.string ());
          cs.append (v.string ());

          appended = true;
        }

        cs.append (p.db.get ().config.string ());
        cs.append (p.name.string ());
        cs.append (p.version->string ());
      }

      if (!appended)
      {
        cs.append (db.config.string ());
        cs.append (n.string ());
        cs.append (v.string ());
      }

      return former_states_.find (cs.string ()) != former_states_.end ();
    }

  private:
    // Return the SHA256 checksum of the current command line state.
    //
    string
    state () const
    {
      // NOTE: remember to update tried_earlier() if changing anything here.
      //
      sha256 cs;
      for (const package_version_key& p: packages_)
      {
        assert (p.version); // Only the real packages can be here.

        cs.append (p.db.get ().config.string ());
        cs.append (p.name.string ());
        cs.append (p.version->string ());
      }

      return cs.string ();
    }

    // Find the command line package spec an adjustment applies to.
    //
    vector<build_package>::iterator
    find_hold_pkg (const cmdline_adjustment& a) const
    {
      return find_if (hold_pkgs_.begin (), hold_pkgs_.end (),
                      [&a] (const build_package& p)
                      {
                        return p.name () == a.name && p.db == a.db;
                      });
    }

    dependency_packages::iterator
    find_dep_pkg (const cmdline_adjustment& a) const
    {
      return find_if (dep_pkgs_.begin (), dep_pkgs_.end (),
                      [&a] (const dependency_package& p)
                      {
                        return p.name == a.name  &&
                               p.db   != nullptr &&
                               *p.db  == a.db;
                      });
    }

  private:
    vector<build_package>& hold_pkgs_;
    dependency_packages&   dep_pkgs_;

    vector<cmdline_adjustment> adjustments_;   // Adjustments stack.
    set<package_version_key>   packages_;      // Replacements.
    set<string>                former_states_; // Command line seen states.
  };

  // Try to replace a collected package with a different available version,
  // satisfactory for all its new and/or existing dependents. Return the
  // command line adjustment if such a replacement is deduced and nullopt
  // otherwise. In the latter case, also return the list of the being built
  // dependents which are unsatisfied by some of the dependency available
  // versions (unsatisfied_dpts argument).
  //
  // Specifically, try to find the best available package version considering
  // all the imposed constraints as per unsatisfied_dependents description. If
  // succeed, return the command line adjustment reflecting the replacement.
  // If allow_downgrade is false, then don't return a downgrade adjustment for
  // the package, unless it is being deorphaned.
  //
  // Notes:
  //
  // - Doesn't perform the actual adjustment of the command line.
  //
  // - Expected to be called after the execution plan is fully refined. That,
  //   in particular, means that all the existing dependents are also
  //   collected and thus the constraints they impose are already in their
  //   dependencies' constraints lists.
  //
  // - The specified package version may or may not be satisfactory for its
  //   new and existing dependents.
  //
  // - The replacement is denied in the following cases:
  //
  //   - If it turns out that the package have been specified on the command
  //     line (by the user or by us on some previous iteration) with an exact
  //     version constraint, then we cannot try any other version.
  //
  //   - If the dependency is system, then it is either specified with the
  //     wildcard version or its exact version have been specified by the user
  //     or have been deduced by the system package manager. In the former
  //     case we actually won't be calling this function for this package
  //     since the wildcard version satisfies any constraint. Thus, an exact
  //     version has been specified/deduced for this dependency and so we
  //     cannot try any other version.
  //
  //   - If the dependency is being built as an existing archive/directory,
  //     then its version is determined and so we cannot try any other
  //     version.
  //
  //   - If the package is already configured with the version held and the
  //     user didn't specify this package on the command line and it is not
  //     requested to be upgraded, patched, and/or deorphaned, then we
  //     shouldn't be silently up/down-grading it.
  //
  static optional<cmdline_adjustment>
  try_replace_dependency (const common_options& o,
                          const build_package& p,
                          bool allow_downgrade,
                          const build_packages& pkgs,
                          const vector<build_package>& hold_pkgs,
                          const dependency_packages& dep_pkgs,
                          const cmdline_adjustments& cmdline_adjs,
                          vector<package_key>& unsatisfied_dpts,
                          const char* what)
  {
    tracer trace ("try_replace_dependency");

    assert (p.available != nullptr); // By definition.

    // Bail out for the system package build.
    //
    if (p.system)
    {
      l5 ([&]{trace << "replacement of " << what << " version "
                    << p.available_name_version_db () << " is denied "
                    << "since it is being configured as system";});

      return nullopt;
    }

    // Bail out for an existing package archive/directory.
    //
    database& db (p.db);
    const package_name& nm (p.name ());
    const version& ver (p.available->version);

    if (find_existing (db,
                       nm,
                       nullopt /* version_constraint */).first != nullptr)
    {
      l5 ([&]{trace << "replacement of " << what << " version "
                    << p.available_name_version_db () << " is denied since "
                    << "it is being built as existing archive/directory";});

      return nullopt;
    }

    // Find the package command line entry and stash the reference to its
    // version constraint, if any. Bail out if the constraint is specified as
    // an exact package version.
    //
    const build_package*      hold_pkg   (nullptr);
    const dependency_package* dep_pkg    (nullptr);
    const version_constraint* constraint (nullptr);

    for (const build_package& hp: hold_pkgs)
    {
      if (hp.name () == nm && hp.db == db)
      {
        hold_pkg = &hp;

        if (!hp.constraints.empty ())
        {
          // Can only contain the user-specified constraint.
          //
          assert (hp.constraints.size () == 1);

          const version_constraint& c (hp.constraints[0].value);

          if (c.min_version == c.max_version)
          {
            l5 ([&]{trace << "replacement of " << what << " version "
                          << p.available_name_version_db () << " is denied "
                          << "since it is specified on command line as '"
                          << nm << ' ' << c << "'";});

            return nullopt;
          }
          else
            constraint = &c;
        }

        break;
      }
    }

    if (hold_pkg == nullptr)
    {
      for (const dependency_package& dp: dep_pkgs)
      {
        if (dp.name == nm && dp.db != nullptr && *dp.db == db)
        {
          dep_pkg = &dp;

          if (dp.constraint)
          {
            const version_constraint& c (*dp.constraint);

            if (c.min_version == c.max_version)
            {
              l5 ([&]{trace << "replacement of " << what << " version "
                            << p.available_name_version_db () << " is denied "
                            << "since it is specified on command line as '?"
                            << nm << ' ' << c << "'";});

              return nullopt;
            }
            else
              constraint = &c;
          }

          break;
        }
      }
    }

    // Bail out if the selected package version is held and the package is not
    // specified on the command line nor is being upgraded/deorphaned via its
    // dependents recursively.
    //
    const shared_ptr<selected_package>& sp (p.selected);

    if (sp != nullptr && sp->hold_version         &&
        hold_pkg == nullptr && dep_pkg == nullptr &&
        !p.upgrade && !p.deorphan)
    {
      l5 ([&]{trace << "replacement of " << what << " version "
                    << p.available_name_version_db () << " is denied since "
                    << "it is already built to hold version and it is not "
                    << "specified on command line nor is being upgraded or "
                    << "deorphaned";});

      return nullopt;
    }

    transaction t (db);

    // Collect the repository fragments to search the available packages in.
    //
    config_repo_fragments rfs;

    // Add a repository fragment to the specified list, suppressing duplicates.
    //
    auto add = [] (shared_ptr<repository_fragment>&& rf,
                   vector<shared_ptr<repository_fragment>>& rfs)
    {
      if (find (rfs.begin (), rfs.end (), rf) == rfs.end ())
        rfs.push_back (move (rf));
    };

    // If the package is specified as build-to-hold on the command line, then
    // collect the root repository fragment from its database. Otherwise,
    // collect the repository fragments its dependent packages come from.
    //
    if (hold_pkg != nullptr)
    {
      add (db.find<repository_fragment> (empty_string), rfs[db]);
    }
    else
    {
      // Collect the repository fragments the new dependents come from.
      //
      if (p.required_by_dependents)
      {
        for (const package_version_key& dvk: p.required_by)
        {
          if (dvk.version) // Real package?
          {
            const build_package* d (pkgs.entered_build (dvk.db, dvk.name));

            // Must be collected as a package build (see
            // build_package::required_by for details).
            //
            assert (d != nullptr                       &&
                    d->action                          &&
                    *d->action == build_package::build &&
                    d->available != nullptr);

            for (const package_location& pl: d->available->locations)
            {
              const lazy_shared_ptr<repository_fragment>& lrf (
                pl.repository_fragment);

              // Note that here we also handle dependents fetched/unpacked
              // using the existing archive/directory adding the root
              // repository fragments from their configurations.
              //
              if (!rep_masked_fragment (lrf))
                add (lrf.load (), rfs[lrf.database ()]);
            }
          }
        }
      }

      // Collect the repository fragments the existing dependents come from.
      //
      // Note that all the existing dependents are already in the map (since
      // collect_dependents() has already been called) and are either
      // reconfigure adjustments or non-collected recursively builds.
      //
      if (sp != nullptr)
      {
        for (database& ddb: db.dependent_configs ())
        {
          for (const auto& pd: query_dependents (ddb, nm, db))
          {
            const build_package* d (pkgs.entered_build (ddb, pd.name));

            // See collect_dependents() for details.
            //
            assert (d != nullptr && d->action);

            if ((*d->action == build_package::adjust &&
                 (d->flags & build_package::adjust_reconfigure) != 0) ||
                (*d->action == build_package::build && !d->dependencies))
            {
              shared_ptr<selected_package> p (
                ddb.load<selected_package> (pd.name));

              add_dependent_repo_fragments (ddb, p, rfs);
            }
          }
        }
      }
    }

    // Query the dependency available packages from all the collected
    // repository fragments and select the most appropriate one. Note that
    // this code is inspired by the evaluate_dependency() function
    // implementation, which documents the below logic in great detail.
    //
    optional<version_constraint> c (constraint != nullptr
                                    ? *constraint
                                    : optional<version_constraint> ());

    if (!c && p.upgrade && !*p.upgrade)
    {
      assert (sp != nullptr); // See build_package::upgrade.

      c = patch_constraint (sp);

      assert (c); // See build_package::upgrade.
    }

    available_packages afs (find_available (nm, c, rfs));

    using available = pair<shared_ptr<available_package>,
                           lazy_shared_ptr<repository_fragment>>;

    available ra;

    // Version to deorphan.
    //
    const version* dov (p.deorphan ? &sp->version : nullptr);

    optional<version_constraint> dopc; // Patch constraint for the above.
    optional<version_constraint> domc; // Minor constraint for the above.

    bool orphan_best_match (p.deorphan && constraint == nullptr && !p.upgrade);

    if (orphan_best_match)
    {
      // Note that non-zero iteration makes a version non-standard, so we
      // reset it to 0 to produce the patch/minor constraints.
      //
      version v (dov->epoch,
                 dov->upstream,
                 dov->release,
                 dov->revision,
                 0 /* iteration */);

      dopc = patch_constraint (nm, v, true /* quiet */);
      domc = minor_constraint (nm, v, true /* quiet */);
    }

    available deorphan_latest_iteration;
    available deorphan_later_revision;
    available deorphan_later_patch;
    available deorphan_later_minor;
    available deorphan_latest_available;

    // Return true if a version satisfies all the dependency constraints.
    // Otherwise, save all the being built unsatisfied dependents into the
    // resulting list, suppressing duplicates.
    //
    auto satisfactory = [&p, &unsatisfied_dpts] (const version& v)
    {
      bool r (true);

      for (const auto& c: p.constraints)
      {
        if (!satisfies (v, c.value))
        {
          r = false;

          if (c.dependent.version && !c.existing_dependent)
          {
            package_key pk (c.dependent.db, c.dependent.name);

            if (find (unsatisfied_dpts.begin (),
                      unsatisfied_dpts.end (),
                      pk) == unsatisfied_dpts.end ())
              unsatisfied_dpts.push_back (move (pk));
          }
        }
      }

      return r;
    };

    for (available& af: afs)
    {
      shared_ptr<available_package>& ap (af.first);

      if (ap->stub ())
        continue;

      const version& av (ap->version);

      // Skip if the available package version doesn't satisfy all the
      // constraints (note: must be checked first since has a byproduct).
      //
      if (!satisfactory (av))
        continue;

      // Don't offer to replace to the same version.
      //
      if (av == ver)
        continue;

      // Don't repeatedly offer the same adjustments for the same command
      // line.
      //
      if (cmdline_adjs.tried_earlier (db, nm, av))
      {
        l5 ([&]{trace << "replacement " << package_version_key (db, nm, av)
                      << " tried earlier for same command line, skipping";});

        continue;
      }

      // If we aim to upgrade to the latest version and it tends to be less
      // then the selected one, then what we currently have is the best that
      // we can get. Thus, we use the selected version as a replacement,
      // unless it doesn't satisfy all the constraints or we are deorphaning.
      // Bail out if we cannot stay with the selected version and downgrade is
      // not allowed.
      //
      if (constraint == nullptr && sp != nullptr)
      {
        const version& sv (sp->version);
        if (av < sv && !p.deorphan)
        {
          // Only consider to keep the selected non-system package if its
          // version is satisfactory for its new dependents (note: must be
          // checked first since has a byproduct), differs from the version
          // being replaced, and was never used for the same command line (see
          // above for details).
          //
          if (!sp->system () && satisfactory (sv) && sv != ver)
          {
            if (!cmdline_adjs.tried_earlier (db, nm, sv))
            {
              ra = make_available_fragment (o, db, sp);
              break;
            }
            else
              l5 ([&]{trace << "selected package replacement "
                            << package_version_key (db, nm, sv) << " tried "
                            << "earlier for same command line, skipping";});
          }

          if (!allow_downgrade)
          {
            l5 ([&]{trace << "downgrade for "
                          << package_version_key (db, nm, sv) << " is not "
                          << "allowed, bailing out";});

            break;
          }
        }
      }

      if (orphan_best_match)
      {
        if (av == *dov)
        {
          ra = move (af);
          break;
        }

        if (deorphan_latest_iteration.first == nullptr &&
            av.compare (*dov, false /* revision */, true /* iteration */) == 0)
          deorphan_latest_iteration = af;

        if (deorphan_later_revision.first == nullptr    &&
            av.compare (*dov, true /* revision */) == 0 &&
            av.compare (*dov, false /* revision */, true /* iteration */) > 0)
          deorphan_later_revision = af;

        if (deorphan_later_patch.first == nullptr &&
            dopc && satisfies (av, *dopc)         &&
            av.compare (*dov, true /* revision */) > 0) // Patch is greater?
          deorphan_later_patch = af;

        if (deorphan_later_minor.first == nullptr      &&
            domc && satisfies (av, *domc)              &&
            av.compare (*dov, true /* revision */) > 0 &&
            deorphan_later_patch.first == nullptr)
          deorphan_later_minor = af;

        if (deorphan_latest_available.first == nullptr)
          deorphan_latest_available = move (af);

        if (av.compare (*dov, false /* revision */, true /* iteration */) < 0)
        {
          assert (deorphan_latest_iteration.first != nullptr ||
                  deorphan_later_revision.first != nullptr   ||
                  deorphan_later_patch.first != nullptr      ||
                  deorphan_later_minor.first != nullptr      ||
                  deorphan_latest_available.first != nullptr);

          break;
        }
      }
      else
      {
        ra = move (af);
        break;
      }
    }

    shared_ptr<available_package>& rap (ra.first);

    if (rap == nullptr && orphan_best_match)
    {
      if (deorphan_latest_iteration.first != nullptr)
        ra = move (deorphan_latest_iteration);
      else if (deorphan_later_revision.first != nullptr)
        ra = move (deorphan_later_revision);
      else if (deorphan_later_patch.first != nullptr)
        ra = move (deorphan_later_patch);
      else if (deorphan_later_minor.first != nullptr)
        ra = move (deorphan_later_minor);
      else if (deorphan_latest_available.first != nullptr)
        ra = move (deorphan_latest_available);
    }

    t.commit ();

    // Bail out if no appropriate replacement is found and return the
    // command line adjustment object otherwise.
    //
    if (rap == nullptr)
      return nullopt;

    optional<cmdline_adjustment> r;

    lazy_shared_ptr<repository_fragment>& raf (ra.second);

    if (hold_pkg != nullptr || dep_pkg != nullptr) // Specified on command line?
    {
      if (hold_pkg != nullptr)
      {
        r = cmdline_adjustment (hold_pkg->db,
                                hold_pkg->name (),
                                move (rap),
                                move (raf));

        if (constraint != nullptr)
        {
          l5 ([&]{trace << "replace " << what << " version "
                        << p.available_name_version () << " with "
                        << r->version << " by overwriting constraint "
                        << cmdline_adjs.to_string (*r) << " on command line";});
        }
        else
        {
          l5 ([&]{trace << "replace " << what << " version "
                        << p.available_name_version () << " with "
                        << r->version << " by adding constraint "
                        << cmdline_adjs.to_string (*r) << " on command line";});
        }
      }
      else // dep_pkg != nullptr
      {
        r = cmdline_adjustment (*dep_pkg->db, dep_pkg->name, rap->version);

        if (constraint != nullptr)
        {
          l5 ([&]{trace << "replace " << what << " version "
                        << p.available_name_version () << " with "
                        << r->version << " by overwriting constraint "
                        << cmdline_adjs.to_string (*r) << " on command line";});
        }
        else
        {
          l5 ([&]{trace << "replace " << what << " version "
                        << p.available_name_version () << " with "
                        << r->version << " by adding constraint "
                        << cmdline_adjs.to_string (*r) << " on command line";});
        }
      }
    }
    else // The package is not specified on the command line.
    {
      // If the package is configured as system, then since it is not
      // specified by the user (both hold_pkg and dep_pkg are NULL) we may
      // only build it as system. Thus we wouldn't be here (see above).
      //
      assert (sp == nullptr || !sp->system ());

      // Similar to the collect lambda in collect_build_prerequisites(), issue
      // the warning if we are forcing an up/down-grade.
      //
      if (sp != nullptr && (sp->hold_package || verb >= 2))
      {
        const version& av (rap->version);
        const version& sv (sp->version);

        int ud (sv.compare (av));

        if (ud != 0)
        {
          for (const auto& c: p.constraints)
          {
            if (c.dependent.version && !satisfies (sv, c.value))
            {
              warn << "package " << c.dependent << " dependency on ("
                   << nm << ' ' << c.value << ") is forcing "
                   << (ud < 0 ? "up" : "down") << "grade of " << *sp << db
                   << " to " << av;

              break;
            }
          }
        }
      }

      // For the selected built-to-hold package create the build-to-hold
      // package spec and the dependency spec otherwise.
      //
      if (sp != nullptr && sp->hold_package)
      {
        r = cmdline_adjustment (db,
                                nm,
                                move (rap),
                                move (raf),
                                p.upgrade,
                                p.deorphan);

        l5 ([&]{trace << "replace " << what << " version "
                      << p.available_name_version () << " with " << r->version
                      << " by adding package spec "
                      << cmdline_adjs.to_string (*r)
                      << " to command line";});
      }
      else
      {
        r = cmdline_adjustment (db, nm, rap->version, p.upgrade, p.deorphan);

        l5 ([&]{trace << "replace " << what << " version "
                      << p.available_name_version () << " with " << r->version
                      << " by adding package spec "
                      << cmdline_adjs.to_string (*r)
                      << " to command line";});
      }
    }

    return r;
  }

  // Try to replace some of the being built, potentially indirect, dependents
  // of the specified dependency with a different available version,
  // satisfactory for all its new and existing dependents (if any). Return the
  // command line adjustment if such a replacement is deduced and nullopt
  // otherwise. If allow_downgrade is false, then don't return a downgrade
  // adjustment, except for a being deorphaned dependent. It is assumed that
  // the dependency replacement has been (unsuccessfully) tried by using the
  // try_replace_dependency() call and its resulting list of the dependents,
  // unsatisfied by some of the dependency available versions, is also passed
  // to the function call as the unsatisfied_dpts argument.
  //
  // Specifically, try to replace the dependents in the following order by
  // calling try_replace_dependency() for them:
  //
  // - Immediate dependents unsatisfied with the specified dependency. For the
  //   sake of tracing and documentation, we (naturally) call them unsatisfied
  //   dependents.
  //
  // - Immediate dependents satisfied with the dependency but applying the
  //   version constraint which has prevented us from picking a version which
  //   would be satisfactory to the unsatisfied dependents. Note that this
  //   information is only available for the being built unsatisfied
  //   dependents (added by collect_build() rather than collect_dependents()).
  //   We call them conflicting dependents.
  //
  // - Immediate dependents which apply constraint to this dependency,
  //   incompatible with constraints of some other dependents (both new and
  //   existing). We call them unsatisfiable dependents.
  //
  // - Immediate dependents from unsatisfied_dpts argument. We call them
  //   constraining dependents.
  //
  // - Dependents of all the above types of dependents, discovered by
  //   recursively calling try_replace_dependent() for them.
  //
  static optional<cmdline_adjustment>
  try_replace_dependent (const common_options& o,
                         const build_package& p, // Dependency.
                         bool allow_downgrade,
                         const vector<unsatisfied_constraint>* ucs,
                         const build_packages& pkgs,
                         const cmdline_adjustments& cmdline_adjs,
                         const vector<package_key>& unsatisfied_dpts,
                         vector<build_package>& hold_pkgs,
                         dependency_packages& dep_pkgs,
                         set<const build_package*>& visited_dpts)
  {
    tracer trace ("try_replace_dependent");

    // Bail out if the dependent has already been visited and add it to the
    // visited set otherwise.
    //
    if (!visited_dpts.insert (&p).second)
      return nullopt;

    using constraint_type = build_package::constraint_type;

    const shared_ptr<available_package>& ap (p.available);
    assert (ap != nullptr); // By definition.

    const version& av (ap->version);

    // List of the dependents which we have (unsuccessfully) tried to replace
    // together with the lists of the constraining dependents.
    //
    vector<pair<package_key, vector<package_key>>> dpts;

    // Try to replace a dependent, unless we have already tried to replace it.
    //
    auto try_replace = [&o,
                        &p,
                        allow_downgrade,
                        &pkgs,
                        &cmdline_adjs,
                        &hold_pkgs,
                        &dep_pkgs,
                        &visited_dpts,
                        &dpts,
                        &trace] (package_key dk, const char* what)
      -> optional<cmdline_adjustment>
    {
      if (find_if (dpts.begin (), dpts.end (),
                   [&dk] (const auto& v) {return v.first == dk;}) ==
          dpts.end ())
      {
        const build_package* d (pkgs.entered_build (dk));

        // Always come from the dependency's constraints member.
        //
        assert (d != nullptr);

        // Skip the visited dependents since, by definition, we have already
        // tried to replace them.
        //
        if (find (visited_dpts.begin (), visited_dpts.end (), d) ==
            visited_dpts.end ())
        {
          l5 ([&]{trace << "try to replace " << what << ' '
                        << d->available_name_version_db () << " of dependency "
                        << p.available_name_version_db () << " with some "
                        << "other version";});

          vector<package_key> uds;

          if (optional<cmdline_adjustment> a = try_replace_dependency (
                o,
                *d,
                allow_downgrade,
                pkgs,
                hold_pkgs,
                dep_pkgs,
                cmdline_adjs,
                uds,
                what))
          {
            return a;
          }

          dpts.emplace_back (move (dk), move (uds));
        }
      }

      return nullopt;
    };

    // Try to replace unsatisfied dependents.
    //
    for (const constraint_type& c: p.constraints)
    {
      const package_version_key& dvk (c.dependent);

      if (dvk.version && !c.existing_dependent && !satisfies (av, c.value))
      {
        if (optional<cmdline_adjustment> a = try_replace (
              package_key (dvk.db, dvk.name), "unsatisfied dependent"))
        {
          return a;
        }
      }
    }

    // Try to replace conflicting dependents.
    //
    if (ucs != nullptr)
    {
      for (const unsatisfied_constraint& uc: *ucs)
      {
        const package_version_key& dvk (uc.constraint.dependent);

        if (dvk.version)
        {
          if (optional<cmdline_adjustment> a = try_replace (
                package_key (dvk.db, dvk.name), "conflicting dependent"))
          {
            return a;
          }
        }
      }
    }

    // Try to replace unsatisfiable dependents.
    //
    for (const constraint_type& c1: p.constraints)
    {
      const package_version_key& dvk (c1.dependent);

      if (dvk.version && !c1.existing_dependent)
      {
        const version_constraint& v1 (c1.value);

        bool unsatisfiable (false);
        for (const constraint_type& c2: p.constraints)
        {
          const version_constraint& v2 (c2.value);

          if (!satisfies (v1, v2) && !satisfies (v2, v1))
          {
            unsatisfiable = true;
            break;
          }
        }

        if (unsatisfiable)
        {
          if (optional<cmdline_adjustment> a = try_replace (
                package_key (dvk.db, dvk.name), "unsatisfiable dependent"))
          {
            return a;
          }
        }
      }
    }

    // Try to replace constraining dependents.
    //
    for (const auto& dk: unsatisfied_dpts)
    {
      if (optional<cmdline_adjustment> a = try_replace (
            dk, "constraining dependent"))
      {
        return a;
      }
    }

    // Try to replace dependents of the above dependents, recursively.
    //
    for (const auto& dep: dpts)
    {
      const build_package* d (pkgs.entered_build (dep.first));

      assert (d != nullptr);

      if (optional<cmdline_adjustment> a = try_replace_dependent (
            o,
            *d,
            allow_downgrade,
            nullptr /* unsatisfied_constraints */,
            pkgs,
            cmdline_adjs,
            dep.second,
            hold_pkgs,
            dep_pkgs,
            visited_dpts))
      {
        return a;
      }
    }

    return nullopt;
  }

  // Return false if the plan execution was noop. If unsatisfied dependents
  // are specified then we are in the simulation mode.
  //
  static bool
  execute_plan (const pkg_build_options&,
                build_package_list&,
                unsatisfied_dependents* simulate,
                const function<find_database_function>&);

  using pkg_options = pkg_build_pkg_options;

  static void
  validate_options (const pkg_options& o, const string& pkg)
  {
    diag_record dr;

    if (o.upgrade () && o.patch ())
      dr << fail << "both --upgrade|-u and --patch|-p specified";

    if (o.immediate () && o.recursive ())
      dr << fail << "both --immediate|-i and --recursive|-r specified";

    // The --immediate or --recursive option can only be specified with an
    // explicit --upgrade, --patch, or --deorphan.
    //
    if (const char* n = (o.immediate () ? "--immediate" :
                         o.recursive () ? "--recursive" : nullptr))
    {
      if (!o.upgrade () && !o.patch () && !o.deorphan ())
        dr << fail << n << " requires explicit --upgrade|-u, --patch|-p, or "
                   << "--deorphan";
    }

    if (((o.upgrade_immediate () ? 1 : 0) +
         (o.upgrade_recursive () ? 1 : 0) +
         (o.patch_immediate ()   ? 1 : 0) +
         (o.patch_recursive ()   ? 1 : 0)) > 1)
      dr << fail << "multiple --(upgrade|patch)-(immediate|recursive) "
                 << "specified";

    if (o.deorphan_immediate () && o.deorphan_recursive ())
      dr << fail << "both --deorphan-immediate and --deorphan-recursive "
                 << "specified";

    if (multi_config ())
    {
      if (const char* opt = o.config_name_specified () ? "--config-name" :
                            o.config_id_specified ()   ? "--config-id"   :
                                                         nullptr)
      {
        dr << fail << opt << " specified for multiple current "
                   << "configurations" <<
                info << "use --config-uuid to specify configurations in "
                     << "this mode";
      }
    }

    if (!dr.empty () && !pkg.empty ())
      dr << info << "while validating options for " << pkg;
  }

  static void
  merge_options (const pkg_options& src, pkg_options& dst)
  {
    if (!(dst.recursive () || dst.immediate ()))
    {
      dst.immediate (src.immediate ());
      dst.recursive (src.recursive ());

      // If -r|-i was specified at the package level, then so should
      // -u|-p and --deorphan.
      //
      if (!(dst.upgrade () || dst.patch ()))
      {
        dst.upgrade (src.upgrade ());
        dst.patch   (src.patch ());
      }

      if (!dst.deorphan ())
        dst.deorphan (src.deorphan ());
    }

    if (!(dst.upgrade_immediate () || dst.upgrade_recursive () ||
          dst.patch_immediate ()   || dst.patch_recursive ()))
    {
      dst.upgrade_immediate (src.upgrade_immediate ());
      dst.upgrade_recursive (src.upgrade_recursive ());
      dst.patch_immediate   (src.patch_immediate ());
      dst.patch_recursive   (src.patch_recursive ());
    }

    if (!(dst.deorphan_immediate () || dst.deorphan_recursive ()))
    {
      dst.deorphan_immediate (src.deorphan_immediate ());
      dst.deorphan_recursive (src.deorphan_recursive ());
    }

    dst.dependency (src.dependency () || dst.dependency ());
    dst.keep_out   (src.keep_out ()   || dst.keep_out ());
    dst.disfigure  (src.disfigure ()  || dst.disfigure ());

    if (!dst.checkout_root_specified () && src.checkout_root_specified ())
    {
      dst.checkout_root (src.checkout_root ());
      dst.checkout_root_specified (true);
    }

    dst.checkout_purge (src.checkout_purge () || dst.checkout_purge ());

    if (src.config_id_specified ())
    {
      const vector<uint64_t>& s (src.config_id ());
      vector<uint64_t>&       d (dst.config_id ());
      d.insert (d.end (), s.begin (), s.end ());

      dst.config_id_specified (true);
    }

    if (src.config_name_specified ())
    {
      const strings& s (src.config_name ());
      strings&       d (dst.config_name ());
      d.insert (d.end (), s.begin (), s.end ());

      dst.config_name_specified (true);
    }

    if (src.config_uuid_specified ())
    {
      const vector<uuid>& s (src.config_uuid ());
      vector<uuid>&       d (dst.config_uuid ());
      d.insert (d.end (), s.begin (), s.end ());

      dst.config_uuid_specified (true);
    }
  }

  static bool
  compare_options (const pkg_options& x, const pkg_options& y)
  {
    return x.keep_out ()           == y.keep_out ()           &&
           x.disfigure ()          == y.disfigure ()          &&
           x.dependency ()         == y.dependency ()         &&
           x.upgrade ()            == y.upgrade ()            &&
           x.patch ()              == y.patch ()              &&
           x.deorphan ()           == y.deorphan ()           &&
           x.immediate ()          == y.immediate ()          &&
           x.recursive ()          == y.recursive ()          &&
           x.upgrade_immediate ()  == y.upgrade_immediate ()  &&
           x.upgrade_recursive ()  == y.upgrade_recursive ()  &&
           x.patch_immediate ()    == y.patch_immediate ()    &&
           x.patch_recursive ()    == y.patch_recursive ()    &&
           x.deorphan_immediate () == y.deorphan_immediate () &&
           x.deorphan_recursive () == y.deorphan_recursive () &&
           x.checkout_root ()      == y.checkout_root ()      &&
           x.checkout_purge ()     == y.checkout_purge ();
  }

  int
  pkg_build (const pkg_build_options& o, cli::group_scanner& args)
  {
    tracer trace ("pkg_build");

    dir_paths cs;
    const dir_paths& config_dirs (!o.directory ().empty ()
                                  ? o.directory ()
                                  : cs);

    if (config_dirs.empty ())
      cs.push_back (current_dir);

    l4 ([&]{for (const auto& d: config_dirs) trace << "configuration: " << d;});

    // Make sure that potential stdout writing failures can be detected.
    //
    cout.exceptions (ostream::badbit | ostream::failbit);

    if (o.noop_exit_specified ())
    {
      if (o.print_only ())
        fail << "--noop-exit specified with --print-only";

      // We can probably use build2's --structured-result to support this.
      //
      if (!o.configure_only ())
        fail << "--noop-exit is only supported in --configure-only mode";
    }

    if (o.update_dependent () && o.leave_dependent ())
      fail << "both --update-dependent|-U and --leave-dependent|-L "
           << "specified" <<
        info << "run 'bpkg help pkg-build' for more information";

    if (o.sys_no_query () && o.sys_install ())
      fail << "both --sys-no-query and --sys-install specified" <<
        info << "run 'bpkg help pkg-build' for more information";

    if (!args.more () && !o.upgrade () && !o.patch () && !o.deorphan ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-build' for more information";

    // If multiple current configurations are specified, then open the first
    // one, attach the remaining, verify that their schemas match (which may
    // not be the case if they don't belong to the same linked database
    // cluster), and attach their explicitly linked databases, recursively.
    //
    // Also populates the system repository.
    //
    // @@ Note that currently we don't verify the specified configurations
    //    belong to the same cluster.
    //
    database mdb (config_dirs[0],
                  trace,
                  true         /* pre_attach */,
                  true         /* sys_rep */,
                  dir_paths () /* pre_link */,
                  (config_dirs.size () == 1
                   ? empty_string
                   : '[' + config_dirs[0].representation () + ']'));

    // Command line as a dependent.
    //
    package_version_key cmd_line (mdb, "command line");

    current_configs.push_back (mdb);

    if (config_dirs.size () != 1)
    {
      transaction t (mdb);

      odb::schema_version sv (mdb.schema_version ());
      for (auto i (config_dirs.begin () + 1); i != config_dirs.end (); ++i)
      {
        database& db (mdb.attach (normalize (*i, "configuration"),
                                  true /* sys_rep */));

        if (db.schema_version () != sv)
          fail << "specified configurations belong to different linked "
               << "configuration clusters" <<
            info << mdb.config_orig <<
            info << db.config_orig;

        db.attach_explicit (true /* sys_rep */);

        // Suppress duplicates.
        //
        if (!current (db))
          current_configs.push_back (db);
      }

      t.commit ();
    }

    validate_options (o, ""); // Global package options.

    // Note that the session spans all our transactions. The idea here is that
    // selected_package objects in build_packages below will be cached in this
    // session. When subsequent transactions modify any of these objects, they
    // will modify the cached instance, which means our list will always "see"
    // their updated state.
    //
    // Also note that rep_fetch() and pkg_fetch() must be called in session.
    //
    session ses;

    // Preparse the (possibly grouped) package specs splitting them into the
    // packages and location parts, and also parsing their options and
    // configuration variables.
    //
    // Also collect repository locations for the subsequent fetch, suppressing
    // duplicates. Note that the last repository location overrides the
    // previous ones with the same canonical name.
    //
    // Also note that the dependency specs may not have the repository
    // location specified, since they obtain the repository information via
    // their ultimate dependent configurations.
    //
    // Also collect the databases specified on the command line for the held
    // packages, to later use them as repository information sources for the
    // dependencies. Additionally use the current configurations as repository
    // information sources.
    //
    repo_configs = current_configs;

    struct pkg_spec
    {
      reference_wrapper<database> db;
      string                      packages;
      repository_location         location;
      pkg_options                 options;
      strings                     config_vars;
    };

    vector<pkg_spec> specs;
    {
      // Read the common configuration variables until we reach the "--"
      // separator, eos or an argument. Non-empty variables list should always
      // be terminated with the "--". Furthermore, argument list that contains
      // anything that looks like a variable (has the '=' character) should be
      // preceded with "--".
      //
      strings cvars;
      bool sep (false); // Seen '--'.

      while (args.more ())
      {
        const char* a (args.peek ());

        // If we see the "--" separator, then we are done parsing variables.
        //
        if (strcmp (a, "--") == 0)
        {
          sep = true;
          args.next ();
          break;
        }

        // Bail out if arguments have started. We will perform the validation
        // later (together with the eos case).
        //
        if (strchr (a, '=') == nullptr)
          break;

        string v (args.next ());

        // Make sure this is not an argument having an option group.
        //
        if (args.group ().more ())
          fail << "unexpected options group for configuration variable '"
               << v << "'";

        cvars.push_back (move (trim (v)));
      }

      if (!cvars.empty () && !sep)
        fail << "configuration variables must be separated from packages "
             << "with '--'";

      database_map<vector<repository_location>> locations;

      transaction t (mdb);

      while (args.more ())
      {
        string a (args.next ());

        // Make sure the argument can not be misinterpreted as a configuration
        // variable.
        //
        if (a.find ('=') != string::npos && !sep)
          fail << "unexpected configuration variable '" << a << "'" <<
            info << "use the '--' separator to treat it as a package";

        pkg_options po;

        // Merge the common and package-specific configuration variables
        // (commons go first).
        //
        strings cvs (cvars);

        try
        {
          cli::scanner& ag (args.group ());

          while (ag.more ())
          {
            if (!po.parse (ag) || ag.more ())
            {
              string a (ag.next ());
              if (a.find ('=') == string::npos)
                fail << "unexpected group argument '" << a << "'";

              trim (a);

              if (a[0] == '!')
                fail << "global override in package-specific configuration "
                     << "variable '" << a << "'";

              cvs.push_back (move (a));
            }
          }

          // We have to manually merge global options into local since just
          // initializing local with global and then parsing local may end up
          // with an invalid set (say, both --immediate and --recursive true).
          //
          merge_options (o, po);

          validate_options (po, a);
        }
        catch (const cli::exception& e)
        {
          fail << e << " grouped for argument " << a;
        }

        // Resolve the configuration options into the databases, suppressing
        // duplicates.
        //
        // Note: main database if no --config-* option is specified, unless we
        // are in the multi-config mode, in which case we fail.
        //
        linked_databases dbs;
        auto add_db = [&dbs] (database& db)
        {
          if (find (dbs.begin (), dbs.end (), db) == dbs.end ())
            dbs.push_back (db);
        };

        for (const string& nm: po.config_name ())
        {
          assert (!multi_config ()); // Should have failed earlier.
          add_db (mdb.find_attached (nm));
        }

        for (uint64_t id: po.config_id ())
        {
          assert (!multi_config ()); // Should have failed earlier.
          add_db (mdb.find_attached (id));
        }

        for (const uuid& uid: po.config_uuid ())
        {
          database* db (nullptr);

          for (database& cdb: current_configs)
          {
            if ((db = cdb.try_find_dependency_config (uid)) != nullptr)
              break;
          }

          if (db == nullptr)
            fail << "no configuration with uuid " << uid << " is linked with "
                 << (!multi_config ()
                     ? mdb.config_orig.representation ()
                     : "specified current configurations");

          add_db (*db);
        }

        // Note that unspecified package configuration in the multi-
        // configurations mode is an error, unless this is a system
        // dependency. We, however, do not parse the package scheme at this
        // stage and so delay the potential failure.
        //
        if (dbs.empty ())
          dbs.push_back (mdb);

        if (!a.empty () && a[0] == '?')
        {
          po.dependency (true);
          a.erase (0, 1);
        }

        // If this is a package to hold, then add its databases to the
        // repository information source list, suppressing duplicates.
        //
        if (!po.dependency ())
        {
          for (database& db: dbs)
          {
            if (find (repo_configs.begin (), repo_configs.end (), db) ==
                repo_configs.end ())
              repo_configs.push_back (db);
          }
        }

        // Check if the argument has the [<packages>]@<location> form or looks
        // like a URL. Find the position of <location> if that's the case and
        // set it to string::npos otherwise.
        //
        // Note that we consider '@' to be such a delimiter only if it comes
        // before ":/" (think a URL which could contain its own '@').
        //
        size_t p (0);

        using url_traits = url::traits_type;

        // Skip leading ':' that are not part of a URL.
        //
        while ((p = a.find_first_of ("@:", p)) != string::npos &&
               a[p] == ':'                                     &&
               url_traits::find (a, p) == string::npos)
          ++p;

        if (p != string::npos)
        {
          if (a[p] == ':')
          {
            // The whole thing must be the location.
            //
            p = url_traits::find (a, p) == 0 ? 0 : string::npos;
          }
          else
            p += 1; // Skip '@'.
        }

        // Split the spec into the packages and location parts. Also save the
        // location for the subsequent fetch operation.
        //
        if (p != string::npos)
        {
          string l (a, p);

          if (l.empty ())
            fail << "empty repository location in '" << a << "'";

          if (po.dependency ())
            fail << "unexpected repository location in '?" << a << "'" <<
              info << "repository location cannot be specified for "
                   << "dependencies";

          string pks (p > 1 ? string (a, 0, p - 1) : empty_string);

          for (size_t i (0); i != dbs.size (); ++i)
          {
            database& db (dbs[i]);

            // Search for the repository location in the database before
            // trying to parse it. Note that the straight parsing could
            // otherwise fail, being unable to properly guess the repository
            // type.
            //
            // Also note that the repository location URL is not unique and we
            // can potentially end up with multiple repositories. For example:
            //
            // $ bpkg add git+file:/path/to/git/repo dir+file:/path/to/git/repo
            // $ bpkg build @/path/to/git/repo
            //
            // That's why we pick the repository only if there is exactly one
            // match.
            //
            shared_ptr<repository> r;
            {
              using query = query<repository>;

              // For case-insensitive filesystems (Windows) we need to match
              // the location case-insensitively against the local repository
              // URLs and case-sensitively against the remote ones.
              //
              // Note that the root repository will never be matched, since
              // its location is empty.
              //
              const auto& url (query::location.url);

#ifndef _WIN32
              query q (url == l);
#else
              string u (url.table ());
              u += '.';
              u += url.column ();

              query q (
                (!query::local && url == l) ||
                ( query::local && u + " COLLATE nocase = " + query::_val (l)));
#endif

              auto rs (db.query<repository> (q));
              auto i (rs.begin ());

              if (i != rs.end ())
              {
                r = i.load ();

                // Fallback to parsing the location if several repositories
                // match.
                //
                if (++i != rs.end ())
                  r = nullptr;
              }
            }

            repository_location loc (r != nullptr
                                     ? r->location
                                     : parse_location (l, nullopt /* type */));

            if (!o.no_fetch ())
            {
              auto i (locations.find (db));
              if (i == locations.end ())
                i = locations.insert (db,
                                      vector<repository_location> ()).first;

              auto pr = [&loc] (const repository_location& i) -> bool
              {
                return i.canonical_name () == loc.canonical_name ();
              };

              vector<repository_location>& ls (i->second);
              auto j (find_if (ls.begin (), ls.end (), pr));

              if (j != ls.end ())
                *j = loc;
              else
                ls.push_back (loc);
            }

            // Move the pkg_spec components for the last database on the list,
            // rather then copying them.
            //
            if (i != dbs.size () - 1)
              specs.push_back (pkg_spec {db, pks, move (loc), po, cvs});
            else
              specs.push_back (pkg_spec {db,
                                         move (pks),
                                         move (loc),
                                         move (po),
                                         move (cvs)});
          }
        }
        else
        {
          // Move the pkg_spec components for the last database in the list,
          // rather then copying them.
          //
          for (size_t i (0); i != dbs.size (); ++i)
          {
            database& db (dbs[i]);

            if (i != dbs.size () - 1)
              specs.emplace_back (pkg_spec {db,
                                            a,
                                            repository_location (),
                                            po,
                                            cvs});
            else
              specs.emplace_back (pkg_spec {db,
                                            move (a),
                                            repository_location (),
                                            move (po),
                                            move (cvs)});
          }
        }
      }

      t.commit ();

      // Initialize tmp directories.
      //
      for (database& db: repo_configs)
        init_tmp (db.config_orig);

      // Fetch the repositories in the current configuration.
      //
      // Note that during this build only the repositories information from
      // the main database will be used.
      //
      for (const auto& l: locations)
        rep_fetch (o,
                   l.first,
                   l.second,
                   o.fetch_shallow (),
                   string () /* reason for "fetching ..." */);
    }

    // Now, as repo_configs is filled and the repositories are fetched mask
    // the repositories, if any.
    //
    if (o.mask_repository_specified () || o.mask_repository_uuid_specified ())
      rep_mask (o.mask_repository (),
                o.mask_repository_uuid (),
                current_configs);

    // Expand the package specs into individual package args, parsing them
    // into the package scheme, name, and version constraint components, and
    // also saving associated options and configuration variables.
    //
    // Note that the package specs that have no scheme and location cannot be
    // unambiguously distinguished from the package archive and directory
    // paths. We will save such package arguments unparsed (into the value
    // data member) and will handle them later.
    //
    struct pkg_arg
    {
      // NULL for system dependency with unspecified configuration.
      //
      database*                    db;

      package_scheme               scheme;
      package_name                 name;
      optional<version_constraint> constraint;
      string                       value;
      pkg_options                  options;
      strings                      config_vars;

      // If schema is sys then this member indicates whether the constraint
      // came from the system package manager (not NULL) or user/fallback
      // (NULL).
      //
      const system_package_status* system_status;
    };

    auto arg_parsed = [] (const pkg_arg& a) {return !a.name.empty ();};

    auto arg_sys = [&arg_parsed] (const pkg_arg& a)
    {
      assert (arg_parsed (a));
      return a.scheme == package_scheme::sys;
    };

    auto arg_string = [&arg_parsed, &arg_sys] (const pkg_arg& a,
                                               bool options = true) -> string
    {
      string r (options && a.options.dependency () ? "?" : string ());

      // Quote an argument if empty or contains spaces.
      //
      auto append = [] (const string& a, string& r)
      {
        if (a.empty () || a.find (' ') != string::npos)
          r += '"' + a + '"';
        else
          r += a;
      };

      if (arg_parsed (a))
        r += package_string (a.name,
                             (a.constraint && !wildcard (*a.constraint)
                              ? a.constraint
                              : nullopt),
                             arg_sys (a));
      else
        append (a.value, r);

      if (options)
      {
        // Compose the options string.
        //
        string s;

        auto add_bool = [&s] (const char* o, bool v)
        {
          if (v)
          {
            if (!s.empty ())
              s += ' ';

            s += o;
          }
        };

        auto add_string = [&s, &append] (const char* o, const string& v)
        {
          if (!s.empty ())
            s += ' ';

          s += o;
          s += ' ';
          append (v, s);
        };

        auto add_num = [&add_string] (const char* o, auto v)
        {
          add_string (o, to_string (v));
        };

        const pkg_options& o (a.options);

        add_bool ("--keep-out",           o.keep_out ());
        add_bool ("--disfigure",          o.disfigure ());
        add_bool ("--upgrade",            o.upgrade ());
        add_bool ("--patch",              o.patch ());
        add_bool ("--deorphan",           o.deorphan ());
        add_bool ("--immediate",          o.immediate ());
        add_bool ("--recursive",          o.recursive ());
        add_bool ("--upgrade-immediate",  o.upgrade_immediate ());
        add_bool ("--upgrade-recursive",  o.upgrade_recursive ());
        add_bool ("--patch-immediate",    o.patch_immediate ());
        add_bool ("--patch-recursive",    o.patch_recursive ());
        add_bool ("--deorphan-immediate", o.deorphan_immediate ());
        add_bool ("--deorphan-recursive", o.deorphan_recursive ());

        if (o.checkout_root_specified ())
          add_string ("--checkout-root", o.checkout_root ().string ());

        add_bool ("--checkout-purge", o.checkout_purge ());

        for (const string& nm: o.config_name ())
          add_string ("--config-name", nm);

        for (uint64_t id: o.config_id ())
          add_num ("--config-id", id);

        for (const uuid& uid: o.config_uuid ())
          add_string ("--config-uuid", uid.string ());

        // Compose the option/variable group.
        //
        if (!s.empty () || !a.config_vars.empty ())
        {
          r += " +{ ";

          if (!s.empty ())
            r += s + ' ';

          for (const string& v: a.config_vars)
          {
            append (v, r);
            r += ' ';
          }

          r += '}';
        }
      }

      return r;
    };

    // Figure out the system package version unless explicitly specified and
    // add the system package authoritative information to the database's
    // system repository unless the database is NULL or it already contains
    // authoritative information for this package. Return the figured out
    // system package version as constraint.
    //
    // Note that it is assumed that all the possible duplicates are handled
    // elsewhere/later.
    //
    auto add_system_package = [&o] (database* db,
                                    const package_name& nm,
                                    optional<version_constraint> vc,
                                    const system_package_status* sps,
                                    vector<shared_ptr<available_package>>* stubs)
      -> pair<version_constraint, const system_package_status*>
    {
      if (!vc)
      {
        assert (sps == nullptr);

        // See if we should query the system package manager.
        //
        if (!sys_pkg_mgr)
          sys_pkg_mgr = o.sys_no_query ()
            ? nullptr
            : make_consumption_system_package_manager (o,
                                                       host_triplet,
                                                       o.sys_distribution (),
                                                       o.sys_architecture (),
                                                       o.sys_install (),
                                                       !o.sys_no_fetch (),
                                                       o.sys_yes (),
                                                       o.sys_sudo ());
        if (*sys_pkg_mgr != nullptr)
        {
          system_package_manager& spm (**sys_pkg_mgr);

          // First check the cache.
          //
          optional<const system_package_status*> os (spm.status (nm, nullptr));

          available_packages aps;
          if (!os)
          {
            // If no cache hit, then collect the available packages for the
            // mapping information.
            //
            aps = find_available_all (current_configs, nm);

            // If no source/stub for the package (and thus no mapping), issue
            // diagnostics consistent with other such places unless explicitly
            // allowed by the user.
            //
            if (aps.empty ())
            {
              if (!o.sys_no_stub ())
                fail << "unknown package " << nm <<
                  info << "consider specifying --sys-no-stub or " << nm << "/*";

              // Add the stub package to the imaginary system repository (like
              // the user-specified case below).
              //
              if (stubs != nullptr)
                stubs->push_back (make_shared<available_package> (nm));
            }
          }

          // This covers both our diagnostics below as well as anything that
          // might be issued by status().
          //
          auto df = make_diag_frame (
            [&nm] (diag_record& dr)
            {
              dr << info << "specify " << nm << "/* if package is not "
                 << "installed with system package manager";

              dr << info << "specify --sys-no-query to disable system "
                 << "package manager interactions";
            });

          if (!os)
          {
            os = spm.status (nm, &aps);
            assert (os);
          }

          if ((sps = *os) != nullptr)
            vc = version_constraint (sps->version);
          else
          {
            diag_record dr (fail);

            dr << "no installed " << (o.sys_install () ? "or available " : "")
               << "system package for " << nm;

            if (!o.sys_install ())
              dr << info << "specify --sys-install to try to install it";
          }
        }
        else
          vc = version_constraint (wildcard_version);
      }
      else
      {
        // The system package may only have an exact/wildcard version
        // specified.
        //
        assert (vc->min_version == vc->max_version);

        // For system packages not associated with a specific repository
        // location add the stub package to the imaginary system repository
        // (see below for details).
        //
        if (stubs != nullptr)
          stubs->push_back (make_shared<available_package> (nm));
      }

      if (db != nullptr)
      {
        assert (db->system_repository);

        const system_package* sp (db->system_repository->find (nm));

        // Note that we don't check for the version match here since that's
        // handled by check_dup() lambda at a later stage, which covers both
        // db and no-db cases consistently.
        //
        if (sp == nullptr || !sp->authoritative)
          db->system_repository->insert (nm,
                                         *vc->min_version,
                                         true /* authoritative */,
                                         sps);
      }

      return make_pair (move (*vc), sps);
    };

    // Create the parsed package argument. Issue diagnostics and fail if the
    // package specification is invalid.
    //
    auto arg_package = [&arg_string, &add_system_package]
                       (database* db,
                        package_scheme sc,
                        package_name nm,
                        optional<version_constraint> vc,
                        pkg_options os,
                        strings vs,
                        vector<shared_ptr<available_package>>* stubs = nullptr)
      -> pkg_arg
    {
      assert (!vc || !vc->empty ()); // May not be empty if present.

      if (db == nullptr)
        assert (sc == package_scheme::sys && os.dependency ());

      pkg_arg r {db,
                 sc,
                 move (nm),
                 move (vc),
                 string () /* value */,
                 move (os),
                 move (vs),
                 nullptr /* system_status */};

      // Verify that the package database is specified in the multi-config
      // mode, unless this is a system dependency package.
      //
      if (multi_config ()              &&
          !os.config_uuid_specified () &&
          !(db == nullptr             &&
            sc == package_scheme::sys &&
            os.dependency ()))
        fail << "no configuration specified for " << arg_string (r) <<
          info << "configuration must be explicitly specified for each "
               << "package in multi-configurations mode" <<
          info << "use --config-uuid to specify its configuration";

      switch (sc)
      {
      case package_scheme::sys:
        {
          assert (stubs != nullptr);

          auto sp (add_system_package (db,
                                       r.name,
                                       move (r.constraint),
                                       nullptr /* system_package_status */,
                                       stubs));

          r.constraint = move (sp.first);
          r.system_status = sp.second;
          break;
        }
      case package_scheme::none: break; // Nothing to do.
      }

      return r;
    };

    // Create the unparsed package argument.
    //
    auto arg_raw = [] (database& db,
                       string v,
                       pkg_options os,
                       strings vs) -> pkg_arg
    {
      return pkg_arg {&db,
                      package_scheme::none,
                      package_name (),
                      nullopt /* constraint */,
                      move (v),
                      move (os),
                      move (vs),
                      nullptr /* system_status */};
    };

    vector<pkg_arg> pkg_args;
    {
      // Cache the system stubs to create the imaginary system repository at
      // the end of the package args parsing. This way we make sure that
      // repositories searched for available packages during the parsing are
      // not complemented with the half-cooked imaginary system repository
      // containing packages that appeared on the command line earlier.
      //
      vector<shared_ptr<available_package>> stubs;

      transaction t (mdb);

      // Don't fold the zero revision if building the package from source so
      // that we build the exact X+0 package revision if it is specified.
      //
      auto version_flags = [] (package_scheme sc)
      {
        version::flags r (version::none);
        switch (sc)
        {
        case package_scheme::none: r = version::none;               break;
        case package_scheme::sys:  r = version::fold_zero_revision; break;
        }
        return r;
      };

      // The system package may only be constrained with an exact/wildcard
      // version.
      //
      auto version_only = [] (package_scheme sc)
      {
        bool r (false);
        switch (sc)
        {
        case package_scheme::none: r = false; break;
        case package_scheme::sys:  r = true;  break;
        }
        return r;
      };

      for (pkg_spec& ps: specs)
      {
        if (ps.location.empty ())
        {
          // Parse if it is clear that this is the package name/version,
          // otherwise add unparsed.
          //
          const char* s (ps.packages.c_str ());
          package_scheme sc (parse_package_scheme (s));

          if (sc != package_scheme::none) // Add parsed.
          {
            bool sys (sc == package_scheme::sys);

            package_name n (parse_package_name (s));

            optional<version_constraint> vc (
              parse_package_version_constraint (
                s, sys, version_flags (sc), version_only (sc)));

            pkg_options& o (ps.options);

            // Disregard the (main) database for a system dependency with
            // unspecified configuration.
            //
            bool no_db (sys                         &&
                        o.dependency ()             &&
                        !o.config_name_specified () &&
                        !o.config_id_specified ()   &&
                        !o.config_uuid_specified ());

            pkg_args.push_back (arg_package (no_db ? nullptr : &ps.db.get (),
                                             sc,
                                             move (n),
                                             move (vc),
                                             move (o),
                                             move (ps.config_vars),
                                             &stubs));
          }
          else                           // Add unparsed.
            pkg_args.push_back (arg_raw (ps.db,
                                         move (ps.packages),
                                         move (ps.options),
                                         move (ps.config_vars)));

          continue;
        }

        // Use it both as the package database and the source of the
        // repository information.
        //
        database& pdb (ps.db);

        // Expand the [[<packages>]@]<location> spec. Fail if the repository
        // is not found in this configuration, that can be the case in the
        // presence of --no-fetch option.
        //
        shared_ptr<repository> r (
          pdb.find<repository> (ps.location.canonical_name ()));

        if (r == nullptr)
          fail << "repository '" << ps.location << "' does not exist in this "
               << "configuration";

        // If no packages are specified explicitly (the argument starts with
        // '@' or is a URL) then we select latest versions of all the packages
        // from this repository. Otherwise, we search for the specified
        // packages and versions (if specified) or latest versions (if
        // unspecified) in the repository and its complements (recursively),
        // failing if any of them are not found.
        //
        if (ps.packages.empty ()) // No packages are specified explicitly.
        {
          // Collect the latest package versions.
          //
          map<package_name, version> pvs;

          for (const repository::fragment_type& rf: r->fragments)
          {
            using query = query<repository_fragment_package>;

            for (const auto& rp: pdb.query<repository_fragment_package> (
                   (query::repository_fragment::name ==
                    rf.fragment.load ()->name) +
                   order_by_version_desc (query::package::id.version)))
            {
              const shared_ptr<available_package>& p (rp);

              if (p->stub ()) // Skip stubs.
                continue;

              const package_name& nm (p->id.name);

              if (ps.options.patch ())
              {
                shared_ptr<selected_package> sp (
                  pdb.find<selected_package> (nm));

                // It seems natural in the presence of --patch option to only
                // patch the selected packages and not to build new packages if
                // they are not specified explicitly.
                //
                // @@ Note that the dependencies may be held now, that can be
                //    unexpected for the user, who may think "I only asked to
                //    patch the packages". We probably could keep the hold flag
                //    for the patched packages unless --dependency option is
                //    specified explicitly. Sounds like a complication, so
                //    let's see if it ever becomes a problem.
                //
                // We still save these package names with the special empty
                // version to later issue info messages about them.
                //
                if (sp == nullptr)
                {
                  pvs.emplace (nm, version ());
                  continue;
                }

                optional<version_constraint> c (patch_constraint (sp));

                // Skip the non-patchable selected package. Note that the
                // warning have already been issued in this case.
                //
                // We also skip versions that can not be considered as a
                // patch for the selected package.
                //
                if (!c || !satisfies (p->version, c))
                  continue;
              }

              auto i (pvs.emplace (nm, p->version));

              if (!i.second && i.first->second < p->version)
                i.first->second = p->version;
            }
          }

          // Populate the argument list with the latest package versions.
          //
          // Don't move options and variables as they may be reused.
          //
          for (auto& pv: pvs)
          {
            if (pv.second.empty ()) // Non-existent and so un-patchable?
              info << "package " << pv.first << " is not present in "
                   << "configuration";
            else
              pkg_args.push_back (arg_package (&pdb,
                                               package_scheme::none,
                                               pv.first,
                                               version_constraint (pv.second),
                                               ps.options,
                                               ps.config_vars));
          }
        }
        else // Packages with optional versions in the coma-separated list.
        {
          for (size_t b (0), p; b != string::npos;
               b = p != string::npos ? p + 1 : p)
          {
            // Extract the package.
            //
            p = ps.packages.find (',', b);

            string pkg (ps.packages, b, p != string::npos ? p - b : p);
            const char* s (pkg.c_str ());

            package_scheme sc (parse_package_scheme (s));

            bool sys (sc == package_scheme::sys);

            package_name n (parse_package_name (s));

            optional<version_constraint> vc (
              parse_package_version_constraint (
                s, sys, version_flags (sc), version_only (sc)));

            // Check if the package is present in the repository and its
            // complements, recursively. If the version is not specified then
            // find the latest allowed one.
            //
            // Note that for the system package we don't care about its exact
            // version available from the repository (which may well be a
            // stub). All we need is to make sure that it is present in the
            // repository.
            //
            bool complements (false);

            vector<shared_ptr<repository_fragment>> rfs;
            rfs.reserve (r->fragments.size ());

            for (const repository::fragment_type& rf: r->fragments)
            {
              shared_ptr<repository_fragment> fr (rf.fragment.load ());

              if (!fr->complements.empty ())
                complements = true;

              rfs.push_back (move (fr));
            }

            optional<version_constraint> c;
            shared_ptr<selected_package> sp;

            if (!sys)
            {
              if (!vc)
              {
                if (ps.options.patch () &&
                    (sp = pdb.find<selected_package> (n)) != nullptr)
                {
                  c = patch_constraint (sp);

                  // Skip the non-patchable selected package. Note that the
                  // warning have already been issued in this case.
                  //
                  if (!c)
                    continue;
                }
              }
              else
                c = vc;
            }

            shared_ptr<available_package> ap (
              find_available_one (pdb, n, c, rfs, false /* prereq */).first);

            // Fail if no available package is found or only a stub is
            // available and we are building a source package.
            //
            if (ap == nullptr || (ap->stub () && !sys))
            {
              diag_record dr (fail);

              // If the selected package is loaded then we aim to patch it.
              //
              if (sp != nullptr)
                dr << "patch version for " << *sp << pdb << " is not found in "
                   << r->name;
              else if (ap == nullptr)
                dr << "package " << pkg << " is not found in " << r->name;
              else // Is a stub.
                dr << "package " << pkg << " is not available in source from "
                   << r->name;

              if (complements)
                dr << " or its complements";

              if (sp == nullptr && ap != nullptr) // Is a stub.
                dr << info << "specify "
                           << package_string (n, vc, true /* system */)
                           << " if it is available from the system";
            }

            // Note that for a system package the wildcard version will be set
            // (see arg_package() for details).
            //
            if (!vc && !sys)
              vc = version_constraint (ap->version);

            // Don't move options and variables as they may be reused.
            //
            // Note that this cannot be a system dependency with unspecified
            // configuration since location is specified and so we always pass
            // the database to the constructor.
            //
            pkg_args.push_back (arg_package (&pdb,
                                             sc,
                                             move (n),
                                             move (vc),
                                             ps.options,
                                             ps.config_vars,
                                             &stubs));
          }
        }
      }

      t.commit ();

      imaginary_stubs = move (stubs);
    }

    // List of package configurations specified on the command line.
    //
    vector<package_key> pkg_confs;

    // Separate the packages specified on the command line into to hold and to
    // up/down-grade as dependencies, and save dependents whose dependencies
    // must be upgraded recursively.
    //
    vector<build_package> hold_pkgs;
    dependency_packages   dep_pkgs;
    recursive_packages    rec_pkgs;

    // Note that the command line adjustments which resolve the unsatisfied
    // dependent issue (see unsatisfied_dependents for details) may
    // potentially be sub-optimal, since we do not perform the full
    // backtracking by trying all the possible adjustments and picking the
    // most optimal combination. Instead, we keep collecting adjustments until
    // either the package builds collection succeeds or there are no more
    // adjustment combinations to try (and we don't try all of them). As a
    // result we, for example, may end up with some redundant constraints on
    // the command line just because the respective dependents have been
    // evaluated first. Generally, dropping all the redundant adjustments can
    // potentially be quite time-consuming, since we would need to try
    // dropping all their possible combinations. We, however, will implement
    // the refinement for only the common case (adjustments are independent),
    // trying to drop just one adjustment per the refinement cycle iteration
    // and wait and see how it goes.
    //
    cmdline_adjustments          cmdline_adjs (hold_pkgs, dep_pkgs);

    // If both are present, then we are in the command line adjustments
    // refinement cycle, where cmdline_refine_adjustment is the adjustment
    // being currently dropped and cmdline_refine_index is its index on the
    // stack (as it appears at the beginning of the cycle).
    //
    optional<cmdline_adjustment> cmdline_refine_adjustment;
    optional<size_t>             cmdline_refine_index;

    // If an --upgrade* or --patch* option is used on the command line, then
    // we try to avoid any package downgrades initially. However, if the
    // resolution fails in this mode, we fall back to allowing such
    // downgrades. Without this logic, we may end up downgrading one package
    // in order to upgrade another, which would be incorrect.
    //
    bool cmdline_allow_downgrade (true);

    {
      // Check if the package is a duplicate. Return true if it is but
      // harmless.
      //
      struct sys_package_key // Like package_key but with NULL'able db.
      {
        package_name name;
        database*    db;   // Can be NULL for system dependency.

        sys_package_key (package_name n, database* d)
            : name (move (n)), db (d) {}

        bool
        operator< (const sys_package_key& v) const
        {
          if (int r = name.compare (v.name))
            return r < 0;

          return db != nullptr && v.db != nullptr ? *db < *v.db :
                 db == nullptr && v.db == nullptr ? false       :
                                                    db == nullptr;
        }
      };

      map<sys_package_key, pkg_arg> package_map;

      auto check_dup = [&package_map, &arg_string, &arg_parsed]
                       (const pkg_arg& pa) -> bool
      {
        assert (arg_parsed (pa));

        auto r (package_map.emplace (sys_package_key {pa.name, pa.db}, pa));

        const pkg_arg& a (r.first->second);
        assert (arg_parsed (a));

        // Note that the variable order may matter.
        //
        // @@ Later we may relax this and replace one package argument with
        //    another if they only differ with the version constraint and one
        //    constraint satisfies the other. We will also need to carefully
        //    maintain the above *_pkgs lists.
        //
        if (!r.second &&
            (a.scheme     != pa.scheme                ||
             a.name       != pa.name                  ||
             a.db         != pa.db                    ||
             a.constraint != pa.constraint            ||
             !compare_options (a.options, pa.options) ||
             a.config_vars != pa.config_vars))
          fail << "duplicate package " << pa.name <<
            info << "first mentioned as " << arg_string (a) <<
            info << "second mentioned as " << arg_string (pa);

        return !r.second;
      };

      transaction t (mdb);

      // Return the available package that matches the specified orphan best
      // (see evaluate_dependency() description for details). Also return the
      // repository fragment the package comes from. Return a pair of NULLs if
      // no suitable package has been found.
      //
      auto find_orphan_match =
        [] (const shared_ptr<selected_package>& sp,
            const lazy_shared_ptr<repository_fragment>& root)
      {
        using available = pair<shared_ptr<available_package>,
                               lazy_shared_ptr<repository_fragment>>;

        assert (sp != nullptr);

        const package_name&           n (sp->name);
        const version&                v (sp->version);
        optional<version_constraint> vc {version_constraint (v)};

        // Note that non-zero iteration makes a version non-standard, so we
        // reset it to 0 to produce the patch/minor constraints.
        //
        version vr (v.epoch,
                    v.upstream,
                    v.release,
                    v.revision,
                    0 /* iteration */);

        optional<version_constraint> pc (
          patch_constraint (n, vr, true /* quiet */));

        optional<version_constraint> mc (
          minor_constraint (n, vr, true /* quiet */));

        // Note: explicit revision makes query_available() to always consider
        // revisions (but not iterations) regardless of the revision argument
        // value.
        //
        optional<version_constraint> verc {
          version_constraint (version (v.epoch,
                                       v.upstream,
                                       v.release,
                                       v.revision ? v.revision : 0,
                                       0 /* iteration */))};

        optional<version_constraint> vlc {
          version_constraint (version (v.epoch,
                                       v.upstream,
                                       v.release,
                                       nullopt,
                                       0 /* iteration */))};

        // Find the latest available non-stub package, optionally matching a
        // constraint and considering revision. If a package is found, then
        // cache it together with the repository fragment it comes from and
        // return true.
        //
        available      find_result;
        const version* find_version (nullptr);
        auto find = [&n,
                     &root,
                     &find_result,
                     &find_version] (const optional<version_constraint>& c,
                                     bool revision = false) -> bool
        {
          available r (
            find_available_one (n, c, root, false /* prereq */, revision));

          const shared_ptr<available_package>& ap (r.first);

          if (ap != nullptr && !ap->stub ())
          {
            find_result = move (r);
            find_version = &find_result.first->version;
            return true;
          }
          else
            return false;
        };

        if (// Same version, revision, and iteration.
            //
            find (vc, true)                                       ||
            //
            // Latest iteration of same version and revision.
            //
            find (verc)                                           ||
            //
            // Later revision of same version.
            //
            (find (vlc) &&
             find_version->compare (v,
                                    false /* revision */,
                                    true /* iteration */) > 0)    ||
            //
            // Later patch of same version.
            //
            (pc && find (pc) &&
             find_version->compare (v, true /* revision */) > 0)  ||
            //
            // Later minor of same version.
            //
            (mc && find (mc) &&
             find_version->compare (v, true /* revision */) > 0)  ||
            //
            // Latest available version, including earlier.
            //
            find (nullopt))
        {
          return find_result;
        }

        return available ();
      };

      // Here is what happens here: for unparsed package args we are going to
      // try and guess whether we are dealing with a package archive, package
      // directory, or package name/version by first trying it as an archive,
      // then as a directory, and then assume it is name/version. Sometimes,
      // however, it is really one of the first two but just broken. In this
      // case things are really confusing since we suppress all diagnostics
      // for the first two "guesses". So what we are going to do here is
      // re-run them with full diagnostics if the name/version guess doesn't
      // pan out.
      //
      bool diag (false);
      for (auto i (pkg_args.begin ()); i != pkg_args.end (); )
      {
        pkg_arg&  pa (*i);
        database* pdb (pa.db);

        // Reduce all the potential variations (archive, directory, package
        // name, package name/version) to a single available_package object.
        //
        // Note that the repository fragment is only used for the
        // build-to-hold packages.
        //
        lazy_shared_ptr<repository_fragment> af;
        shared_ptr<available_package> ap;
        bool existing (false); // True if build as an archive or directory.

        if (!arg_parsed (pa))
        {
          assert (pdb != nullptr); // Unparsed and so can't be system.

          lazy_shared_ptr<repository_fragment> root (*pdb, empty_string);

          const char* package (pa.value.c_str ());

          try
          {
            path a (package);
            if (exists (a))
            {
              if (diag)
                info << "'" << package << "' does not appear to be a valid "
                     << "package archive: ";

              package_manifest m (
                pkg_verify (o,
                            a,
                            true /* ignore_unknown */,
                            false /* ignore_toolchain */,
                            false /* expand_values */,
                            true /* load_buildfiles */,
                            true /* complete_values */,
                            diag ? 2 : 1));

              // This is a package archive.
              //
              l4 ([&]{trace << "archive '" << a << "': " << arg_string (pa);});

              pa = arg_package (pdb,
                                package_scheme::none,
                                m.name,
                                version_constraint (m.version),
                                move (pa.options),
                                move (pa.config_vars));

              af = root;
              ap = make_shared<available_package> (move (m));
              ap->locations.push_back (package_location {root, move (a)});

              existing_packages.push_back (make_pair (ref (*pdb), ap));
              existing = true;
            }
          }
          catch (const invalid_path&)
          {
            // Not a valid path so cannot be an archive.
          }
          catch (const not_package&)
          {
          }

          // Is this a package directory?
          //
          // We used to just check any name which led to some really bizarre
          // behavior where a sub-directory of the working directory happened
          // to contain a manifest file and was therefore treated as a package
          // directory. So now we will only do this test if the name ends with
          // the directory separator.
          //
          size_t pn (strlen (package));
          if (pn != 0 && path::traits_type::is_separator (package[pn - 1]))
          {
            try
            {
              dir_path d (package);
              if (exists (d))
              {
                if (diag)
                  info << "'" << package << "' does not appear to be a valid "
                       << "package directory: ";

                // For better diagnostics, let's obtain the package info after
                // pkg_verify() verifies that this is a package directory.
                //
                package_version_info pvi;

                package_manifest m (
                  pkg_verify (
                    o,
                    d,
                    true /* ignore_unknown */,
                    false /* ignore_toolchain */,
                    true /* load_buildfiles */,
                    [&o, &d, &pvi] (version& v)
                    {
                      // Note that we also query subprojects since the package
                      // information will be used for the subsequent
                      // package_iteration() call.
                      //
                      pvi = package_version (o, d, b_info_flags::subprojects);

                      if (pvi.version)
                        v = move (*pvi.version);
                    },
                    diag ? 2 : 1));

                // This is a package directory.
                //
                l4 ([&]{trace << "directory '" << d << "': "
                              << arg_string (pa);});

                // Fix-up the package version to properly decide if we need to
                // upgrade/downgrade the package.
                //
                if (optional<version> v =
                    package_iteration (o,
                                       *pdb,
                                       t,
                                       d,
                                       m.name,
                                       m.version,
                                       &pvi.info,
                                       true /* check_external */))
                  m.version = move (*v);

                pa = arg_package (pdb,
                                  package_scheme::none,
                                  m.name,
                                  version_constraint (m.version),
                                  move (pa.options),
                                  move (pa.config_vars));

                ap = make_shared<available_package> (move (m));
                af = root;
                ap->locations.push_back (package_location {root, move (d)});

                existing_packages.push_back (make_pair (ref (*pdb), ap));
                existing = true;
              }
            }
            catch (const invalid_path&)
            {
              // Not a valid path so cannot be a package directory.
            }
            catch (const not_package&)
            {
            }
          }
        }

        // If this was a diagnostics "run", then we are done.
        //
        if (diag)
          throw failed ();

        // Then it got to be a package name with optional version.
        //
        shared_ptr<selected_package> sp;
        bool patch (false);
        bool deorphan (false);

        if (ap == nullptr)
        {
          try
          {
            if (!arg_parsed (pa))
            {
              const char* package (pa.value.c_str ());

              // Make sure that we can parse both package name and version,
              // prior to saving them into the package arg.
              //
              package_name n (parse_package_name (package));

              // Don't fold the zero revision so that we build the exact X+0
              // package revision, if it is specified.
              //
              optional<version_constraint> vc (
                parse_package_version_constraint (
                  package,
                  false /* allow_wildcard */,
                  version::none));

              pa = arg_package (pdb,
                                package_scheme::none,
                                move (n),
                                move (vc),
                                move (pa.options),
                                move (pa.config_vars));
            }

            l4 ([&]{trace << "package: " << arg_string (pa);});

            if (!pa.options.dependency ())
            {
              assert (pdb != nullptr);

              lazy_shared_ptr<repository_fragment> root (*pdb, empty_string);

              // Get the user-specified version, the latest allowed version,
              // or the orphan best match for a source code package. For a
              // system package we will try to find the available package that
              // matches the user-specified system version (preferable for the
              // configuration negotiation machinery) and, if fail, fallback
              // to picking the latest one just to make sure the package is
              // recognized.
              //
              optional<version_constraint> c;

              bool sys (arg_sys (pa));

              if (!pa.constraint)
              {
                assert (!sys);

                if (pa.options.patch () &&
                    (sp = pdb->find<selected_package> (pa.name)) != nullptr)
                {
                  c = patch_constraint (sp);

                  // Skip the non-patchable selected package. Note that the
                  // warning have already been issued in this case.
                  //
                  if (!c)
                  {
                    ++i;
                    continue;
                  }

                  patch = true;
                }
              }
              else if (!sys || !wildcard (*pa.constraint))
                c = pa.constraint;

              if (pa.options.deorphan ())
              {
                if (!sys)
                {
                  if (sp == nullptr)
                    sp = pdb->find<selected_package> (pa.name);

                  if (sp != nullptr && orphan_package (*pdb, sp))
                    deorphan = true;
                }

                // If the package is not an orphan, its version is not
                // constrained and upgrade/patch is not requested, then just
                // skip the package.
                //
                if (!deorphan              &&
                    !pa.constraint         &&
                    !pa.options.upgrade () &&
                    !pa.options.patch ())
                {
                  ++i;
                  continue;
                }
              }

              pair<shared_ptr<available_package>,
                   lazy_shared_ptr<repository_fragment>> rp (
                     deorphan               &&
                     !pa.constraint         &&
                     !pa.options.upgrade () &&
                     !pa.options.patch ()
                     ? find_orphan_match (sp, root)
                     : find_available_one (pa.name, c, root));

              if (rp.first == nullptr && sys)
              {
                available_packages aps (
                  find_available_all (repo_configs, pa.name));

                if (!aps.empty ())
                  rp = move (aps.front ());
              }

              ap = move (rp.first);
              af = move (rp.second);
            }
          }
          catch (const failed& e)
          {
            assert (e.code == 1);
            diag = true;
            continue;
          }
        }

        // We are handling this argument.
        //
        if (check_dup (*i++))
          continue;

        // Save (both packages to hold and dependencies) as dependents for
        // recursive upgrade/deorphaning.
        //
        {
          // Recursive/immediate upgrade/patch.
          //
          optional<bool> r; // true -- recursive, false -- immediate.
          optional<bool> u; // true -- upgrade,   false -- patch.

          // Recursive/immediate deorphaning.
          //
          optional<bool> d; // true -- recursive, false -- immediate.

          const auto& po (pa.options);

          // Note that, for example, --upgrade-immediate wins over the
          // --upgrade --recursive options pair.
          //
          if (po.immediate ())
          {
            if (po.upgrade () || po.patch ())
            {
              r = false;
              u = po.upgrade ();
            }

            if (po.deorphan ())
              d = false;
          }
          else if (po.recursive ())
          {
            if (po.upgrade () || po.patch ())
            {
              r = true;
              u = po.upgrade ();
            }

            if (po.deorphan ())
              d = true;
          }

          if      (po.upgrade_immediate ()) { u = true;  r = false; }
          else if (po.upgrade_recursive ()) { u = true;  r = true;  }
          else if (  po.patch_immediate ()) { u = false; r = false; }
          else if (  po.patch_recursive ()) { u = false; r = true;  }

          if      (po.deorphan_immediate ()) { d = false; }
          else if (po.deorphan_recursive ()) { d = true;  }

          if (r || d)
          {
            l4 ([&]{trace << "stash recursive package " << arg_string (pa);});

            // The above options are meaningless for system packages, so we
            // just ignore them for a system dependency with unspecified
            // configuration.
            //
            if (pdb != nullptr)
            {
              if (u)
                cmdline_allow_downgrade = false;

              rec_pkgs.push_back (recursive_package {*pdb, pa.name,
                                                     r, u && *u,
                                                     d});
            }
          }
        }

        // Add the dependency package to the list.
        //
        if (pa.options.dependency ())
        {
          l4 ([&]{trace << "stash dependency package " << arg_string (pa);});

          bool sys (arg_sys (pa));

          if (pdb != nullptr)
            sp = pdb->find<selected_package> (pa.name);

          // Make sure that the package is known. Only allow to unhold an
          // unknown orphaned selected package (with the view that there is
          // a good chance it will get dropped; and if not, such an unhold
          // should be harmless).
          //
          if (!existing &&
              find_available (repo_configs,
                              pa.name,
                              !sys ? pa.constraint : nullopt).empty ())
          {
            // Don't fail if the selected package is held and satisfies the
            // constraints, if specified. Note that we may still fail later
            // with the "not available from its dependents' repositories"
            // error if the dependency is requested to be deorphaned and all
            // its dependents are orphaned.
            //
            if (!(sp != nullptr    &&
                  sp->hold_package &&
                  (!pa.constraint || satisfies (sp->version, pa.constraint))))
            {
              string n (arg_string (pa, false /* options */));

              diag_record dr (fail);
              dr << "unknown package " << n;
              if (sys)
              {
                // Feels like we can't end up here if the version was specified
                // explicitly.
                //
                dr << info << "consider specifying " << n << "/*";
              }
              else
                check_any_available (repo_configs, t, &dr);
            }
          }

          if (pdb != nullptr)
            pkg_confs.emplace_back (*pdb, pa.name);

          bool hold_version (pa.constraint.has_value ());

          optional<bool> upgrade (pa.options.upgrade () || pa.options.patch ()
                                  ? pa.options.upgrade ()
                                  : optional<bool> ());

          dep_pkgs.push_back (
            dependency_package {pdb,
                                move (pa.name),
                                move (pa.constraint),
                                hold_version,
                                move (sp),
                                sys,
                                existing,
                                upgrade,
                                pa.options.deorphan (),
                                pa.options.keep_out (),
                                pa.options.disfigure (),
                                (pa.options.checkout_root_specified ()
                                 ? move (pa.options.checkout_root ())
                                 : optional<dir_path> ()),
                                pa.options.checkout_purge (),
                                move (pa.config_vars),
                                pa.system_status});

          if (upgrade)
            cmdline_allow_downgrade = false;

          continue;
        }

        // Add the held package to the list.
        //
        assert (pdb != nullptr);

        lazy_shared_ptr<repository_fragment> root (*pdb, empty_string);

        // Load the package that may have already been selected (if not done
        // yet) and figure out what exactly we need to do here. The end goal
        // is the available_package object corresponding to the actual
        // package that we will be building (which may or may not be
        // the same as the selected package).
        //
        if (sp == nullptr)
          sp = pdb->find<selected_package> (pa.name);

        if (sp != nullptr && sp->state == package_state::broken)
          fail << "unable to build broken package " << pa.name << *pdb <<
            info << "use 'pkg-purge --force' to remove";

        bool found (true);
        bool sys_advise (false);

        bool sys (arg_sys (pa));

        // If the package is not available from the repository we can try to
        // create it from the orphaned selected package. Meanwhile that
        // doesn't make sense for a system package. The only purpose to
        // configure a system package is to build its dependent. But if the
        // package is not in the repository then there is no dependent for it
        // (otherwise the repository would be broken).
        //
        if (!sys)
        {
          // If we failed to find the requested package we can still check if
          // the package name is present in the repositories and if that's the
          // case to inform a user about the possibility to configure the
          // package as a system one on failure. Note we still can end up
          // creating an orphan from the selected package and so succeed.
          //
          if (ap == nullptr)
          {
            if (pa.constraint &&
                find_available_one (pa.name, nullopt, root).first != nullptr)
              sys_advise = true;
          }
          else if (ap->stub ())
          {
            sys_advise = true;
            ap = nullptr;
          }

          // If the user constrained the version, then that's what we ought to
          // be building.
          //
          if (pa.constraint)
          {
            for (;;) // Breakout loop.
            {
              if (ap != nullptr) // Must be that version, see above.
                break;

              // Otherwise, our only chance is that the already selected object
              // satisfies the version constraint, unless we are deorphaning.
              //
              if (sp != nullptr                          &&
                  !sp->system ()                         &&
                  satisfies (sp->version, pa.constraint) &&
                  !deorphan)
                break; // Derive ap from sp below.

              found = false;
              break;
            }
          }
          //
          // No explicit version was specified by the user.
          //
          else
          {
            if (ap != nullptr)
            {
              assert (!ap->stub ());

              // Even if this package is already in the configuration, should
              // we have a newer version, we treat it as an upgrade request;
              // otherwise, why specify the package in the first place? We just
              // need to check if what we already have is "better" (i.e.,
              // newer), unless we are deorphaning.
              //
              if (sp != nullptr             &&
                  !sp->system ()            &&
                  ap->version < sp->version &&
                  !deorphan)
                ap = nullptr; // Derive ap from sp below.
            }
            else
            {
              if (sp == nullptr || sp->system () || deorphan)
                found = false;

              // Otherwise, derive ap from sp below.
            }
          }
        }
        else if (ap == nullptr)
          found = false;

        if (!found)
        {
          // We can always fallback to making available from the selected
          // package.
          //
          assert (!patch);

          diag_record dr (fail);

          if (!sys_advise)
          {
            // Note that if the package is not system and its version was
            // explicitly specified, then we can only be here if no version of
            // this package is available in source from the repository
            // (otherwise we would advise to configure it as a system package;
            // see above). Thus, let's not print it's version constraint in
            // this case.
            //
            // Also note that for a system package we can't end up here if the
            // version was specified explicitly.
            //
            string n (package_string (pa.name, nullopt /* vc */, sys));

            dr << "unknown package " << n;

            // Let's help the new user out here a bit.
            //
            if (sys)
              dr << info << "consider specifying " << n << "/*";
            else
              check_any_available (*pdb, t, &dr);
          }
          else
          {
            assert (!sys);

            dr << arg_string (pa, false /* options */)
               << " is not available in source";

            pa.scheme = package_scheme::sys;

            dr << info << "specify " << arg_string (pa, false /* options */)
                       << " if it is available from the system";
          }
        }

        // If the available_package object is still NULL, then it means
        // we need to get one corresponding to the selected package.
        //
        if (ap == nullptr)
        {
          assert (sp != nullptr && sp->system () == sys);

          auto rp (make_available_fragment (o, *pdb, sp));
          ap = move (rp.first);
          af = move (rp.second); // Could be NULL (orphan).
        }

        // We will keep the output directory only if the external package is
        // replaced with an external one. Note, however, that at this stage
        // the available package is not settled down yet, as we still need to
        // satisfy all the constraints. Thus the available package check is
        // postponed until the package disfiguring.
        //
        bool keep_out (pa.options.keep_out () &&
                       sp != nullptr && sp->external ());

        bool replace ((existing && sp != nullptr) || deorphan);

        // Finally add this package to the list.
        //
        optional<bool> upgrade (sp != nullptr  &&
                                !pa.constraint &&
                                (pa.options.upgrade () || pa.options.patch ())
                                ? pa.options.upgrade ()
                                : optional<bool> ());

        // @@ Pass pa.configure_only() when support for package-specific
        //    --configure-only is added.
        //
        build_package p {
          build_package::build,
          *pdb,
          move (sp),
          move (ap),
          move (af),
          nullopt,                    // Dependencies.
          nullopt,                    // Dependencies alternatives.
          nullopt,                    // Package skeleton.
          nullopt,                    // Postponed dependency alternatives.
          false,                      // Recursive collection.
          true,                       // Hold package.
          pa.constraint.has_value (), // Hold version.
          {},                         // Constraints.
          sys,
          keep_out,
          pa.options.disfigure (),
          false,                      // Configure-only.
          (pa.options.checkout_root_specified ()
           ? move (pa.options.checkout_root ())
           : optional<dir_path> ()),
          pa.options.checkout_purge (),
          move (pa.config_vars),
          upgrade,
          deorphan,
          {cmd_line},                 // Required by (command line).
          false,                      // Required by dependents.
          replace ? build_package::build_replace : uint16_t (0)};

        l4 ([&]{trace << "stash held package "
                      << p.available_name_version_db ();});

        // "Fix" the version the user asked for by adding the constraint.
        //
        // Note: for a system package this must always be present (so that
        // this build_package instance is never replaced).
        //
        if (pa.constraint)
          p.constraints.emplace_back (
            move (*pa.constraint), cmd_line.db, cmd_line.name.string ());

        pkg_confs.emplace_back (p.db, p.name ());

        if (p.upgrade)
          cmdline_allow_downgrade = false;

        hold_pkgs.push_back (move (p));
      }

      // If this is just pkg-build -u|-p, then we are upgrading all held
      // packages.
      //
      // Should we also upgrade the held packages in the explicitly linked
      // configurations, recursively? Maybe later and we probably will need a
      // command line option to enable this behavior.
      //
      if (hold_pkgs.empty () && dep_pkgs.empty () &&
          (o.upgrade () || o.patch () || o.deorphan ()))
      {
        for (database& cdb: current_configs)
        {
          lazy_shared_ptr<repository_fragment> root (cdb, empty_string);

          using query = query<selected_package>;

          for (shared_ptr<selected_package> sp:
                 pointer_result (
                   cdb.query<selected_package> (
                     query::state == "configured" && query::hold_package)))
          {
            // Let's skip upgrading system packages as they are, probably,
            // configured as such for a reason.
            //
            if (sp->system ())
              continue;

            const package_name& name (sp->name);

            optional<version_constraint> pc;

            if (o.patch ())
            {
              pc = patch_constraint (sp);

              // Skip the non-patchable selected package. Note that the
              // warning have already been issued in this case.
              //
              if (!pc)
                continue;
            }

            bool deorphan (false);

            if (o.deorphan ())
            {
              // If the package is not an orphan and upgrade/patch is not
              // requested, then just skip the package.
              //
              if (orphan_package (cdb, sp))
                deorphan = true;
              else if (!o.upgrade () && !o.patch ())
                continue;
            }

            // In the deorphan mode with no upgrade/patch requested pick the
            // version that matches the orphan best. Otherwise, pick the patch
            // or the latest available version, as requested.
            //
            auto apr (deorphan && !o.upgrade () && !o.patch ()
                      ? find_orphan_match (sp, root)
                      : find_available_one (name, pc, root));

            shared_ptr<available_package> ap (move (apr.first));
            if (ap == nullptr || ap->stub ())
            {
              diag_record dr (fail);
              dr << name << " is not available";

              if (ap != nullptr) // Stub?
              {
                dr << " in source" <<
                  info << "consider building it as "
                       << package_string (name, version (), true /* system */)
                       << " if it is available from the system";
              }

              // Let's help the new user out here a bit.
              //
              check_any_available (cdb, t, &dr);
            }

            // We will keep the output directory only if the external package
            // is replaced with an external one (see above for details).
            //
            bool keep_out (o.keep_out () && sp->external ());

            // @@ Pass pa.configure_only() when support for package-specific
            //    --configure-only is added.
            //
            build_package p {
              build_package::build,
              cdb,
              move (sp),
              move (ap),
              move (apr.second),
              nullopt,                      // Dependencies.
              nullopt,                      // Dependencies alternatives.
              nullopt,                      // Package skeleton.
              nullopt,                      // Postponed dependency alternatives.
              false,                        // Recursive collection.
              true,                         // Hold package.
              false,                        // Hold version.
              {},                           // Constraints.
              false,                        // System package.
              keep_out,
              o.disfigure (),
              false,                        // Configure-only.
              nullopt,                      // Checkout root.
              false,                        // Checkout purge.
              strings (),                   // Configuration variables.
              (o.upgrade () || o.patch ()
               ? o.upgrade ()
               : optional<bool> ()),
              deorphan,
              {cmd_line},                   // Required by (command line).
              false,                        // Required by dependents.
              deorphan ? build_package::build_replace : uint16_t (0)};

            l4 ([&]{trace << "stash held package "
                          << p.available_name_version_db ();});

            if (p.upgrade)
              cmdline_allow_downgrade = false;

            hold_pkgs.push_back (move (p));

            // If there are also -i|-r, then we are also upgrading and/or
            // deorphaning dependencies of all held packages.
            //
            if (o.immediate () || o.recursive ())
            {
              rec_pkgs.push_back (recursive_package {
                  cdb, name,
                  (o.upgrade () || o.patch ()
                   ? o.recursive ()
                   : optional<bool> ()),
                  o.upgrade (),
                  (o.deorphan ()
                   ? o.recursive ()
                   : optional<bool> ())});
            }
          }
        }
      }

      t.commit ();
    }

    if (hold_pkgs.empty () && dep_pkgs.empty ())
    {
      assert (rec_pkgs.empty ());

      if (o.noop_exit_specified ())
        return o.noop_exit ();

      info << "nothing to build";
      return 0;
    }

    // Search for the package prerequisite among packages specified on the
    // command line and, if found, return its desired database. Return NULL
    // otherwise. The `db` argument specifies the dependent database.
    //
    // Note that the semantics of a package specified on the command line is:
    // build the package in the specified configuration (current by default)
    // and repoint all dependents in the current configuration of this
    // prerequisite to this new prerequisite. Thus, the function always
    // returns NULL for dependents not in the current configuration.
    //
    // Also note that we rely on "small function object" optimization here.
    //
    const function<find_database_function> find_prereq_database (
      [&pkg_confs] (database& db,
                    const package_name& nm,
                    bool buildtime) -> database*
      {
        database* r (nullptr);

        linked_databases ddbs (db.dependency_configs (nm, buildtime));

        for (const package_key& p: pkg_confs)
        {
          if (p.name == nm &&
              find (ddbs.begin (), ddbs.end (), p.db) != ddbs.end ())
          {
            if (r == nullptr)
              r = &p.db.get ();
            else
              fail << "multiple " << p.db.get ().type << " configurations "
                   << "specified for package " << nm <<
                info << r->config_orig <<
                info << p.db.get ().config_orig;
          }
        }

        return r;
      });

    // Assemble the list of packages we will need to build-to-hold, still used
    // dependencies to up/down-grade, and unused dependencies to drop. We call
    // this the plan.
    //
    // Note: for the sake of brevity we also assume the package replacement
    // wherever we mention the package up/down-grade in this description.
    //
    // The way we do it is tricky: we first create the plan based on build-to-
    // holds (i.e., the user selected). Next, to decide whether we need to
    // up/down-grade or drop any dependecies we need to take into account an
    // existing state of the package database plus the changes that would be
    // made to it once we executed the plan (think about it: when the user
    // says "I want to upgrade a package and all its dependencies", they want
    // to upgrade dependencies that are still used after upgrading the
    // package, not those that were used before by the old version).
    //
    // As you can probably imagine, figuring out the desired state of the
    // dependencies based on the current package database and to-be-executed
    // plan won't be an easy task. So instead what we are going to do is
    // simulate the plan execution by only applying it to the package database
    // (but not to the filesystem/packages themselves). We then use this
    // simulated database as the sole (and convenient) source of the
    // dependency information (i.e., which packages are still used and by
    // whom) to decide which dependencies we need to upgrade, downgrade, or
    // drop. Once this is done, we rollback the database (and reload any
    // in-memory objects that might have changed during the simulation) and
    // add the up/down-grades and drops to the plan.
    //
    // Of course, adding dependency up/down-grade to the plan can change the
    // plan. For example, a new version of a dependency we are upgrading may
    // force an upgrade of one of the packages from the user selected. And
    // that, in turn, can pretty much rewrite the plan entirely (including
    // rendering our earlier decisions about up/down-grades/drops of other
    // dependencies invalid).
    //
    // So what we have to do is refine the plan over several iterations.
    // Specifically, if we added a new up/down-grade/drop, then we need to
    // re-simulate this plan and (1) re-example if any new dependencies now
    // need up/down-grade/drop and, this one is tricky, (2) that none of the
    // already made decisions have changed. If we detect (2), then we need to
    // cancel all such decisions and also rebuild the plan from scratch. The
    // intuitive feeling here is that this process will discover an up/down-
    // grade order where any subsequent entry does not affect the decision of
    // the previous ones.
    //
    // Note that we also need to rebuild the plan from scratch on adding a new
    // up/down-grade/drop if any dependency configuration negotiation has been
    // performed, since any package replacement may affect the already
    // negotiated configurations.
    //
    // Package managers are an easy, already solved problem, right?
    //
    build_packages pkgs;
    {
      struct dep
      {
        reference_wrapper<database> db;
        package_name                name; // Empty if up/down-grade.

        // Both are NULL if drop.
        //
        shared_ptr<available_package>              available;
        lazy_shared_ptr<bpkg::repository_fragment> repository_fragment;

        bool           system;
        bool           existing; // Build as an existing archive or directory.
        optional<bool> upgrade;
        bool           deorphan;
      };
      vector<dep> deps;
      existing_dependencies   existing_deps;
      deorphaned_dependencies deorphaned_deps;

      replaced_versions         replaced_vers;
      postponed_dependencies    postponed_deps;
      unacceptable_alternatives unacceptable_alts;

      // Map the repointed dependents to the replacement flags (see
      // repointed_dependents for details), unless --no-move is specified.
      //
      // Note that the overall plan is to add the replacement prerequisites to
      // the repointed dependents prerequisites sets at the beginning of the
      // refinement loop iteration and remove them right before the plan
      // execution simulation. This will allow the collecting/ordering
      // functions to see both kinds of prerequisites (being replaced and
      // their replacements) and only consider one kind or another or both, as
      // appropriate.
      //
      repointed_dependents rpt_depts;

      if (!o.no_move ())
      {
        transaction t (mdb);

        using query = query<selected_package>;

        query q (query::state == "configured");

        for (database& cdb: current_configs)
        {
          for (shared_ptr<selected_package> sp:
                 pointer_result (cdb.query<selected_package> (q)))
          {
            map<package_key, bool> ps; // Old/new prerequisites.

            for (const auto& p: sp->prerequisites)
            {
              database& db (p.first.database ());
              const package_name& name (p.first.object_id ());

              // Note that if a prerequisite is in a configuration of the host
              // type, it is not necessarily a build-time dependency (think of
              // a dependent from a self-hosted configuration and its runtime
              // dependency). However, here it doesn't really matter.
              //
              database* pdb (
                find_prereq_database (cdb,
                                      name,
                                      (db.type == host_config_type ||
                                       db.type == build2_config_type)));

              if (pdb != nullptr && *pdb != db && pdb->type == db.type)
              {
                ps.emplace (package_key {*pdb, name}, true);
                ps.emplace (package_key {  db, name}, false);
              }
            }

            if (!ps.empty ())
              rpt_depts.emplace (package_key {cdb, sp->name}, move (ps));
          }
        }

        t.commit ();
      }

      // Iteratively refine the plan with dependency up/down-grades/drops.
      //
      // Note that we should not clean the deps list on scratch_col (scratch
      // during the package collection) because we want to enter them before
      // collect_build_postponed() and they could be the dependents that have
      // the config clauses. In a sense, change to replaced_vers,
      // postponed_deps, or unacceptable_alts maps should not affect the deps
      // list. But not the other way around: a dependency erased from the deps
      // list could have caused an entry in the replaced_vers, postponed_deps,
      // and/or unacceptable_alts maps. And so we clean replaced_vers,
      // postponed_deps, and unacceptable_alts on scratch_exe (scratch during
      // the plan execution).
      //
      for (bool refine (true), scratch_exe (true), scratch_col (false);
           refine; )
      {
        bool scratch (scratch_exe || scratch_col);

        l4 ([&]{trace << "refine package collection/plan execution"
                      << (scratch ? " from scratch" : "");});

        transaction t (mdb);

        // Collect all configurations where dependency packages can
        // potentially be built or amended during this run.
        //
        linked_databases dep_dbs;

        for (database& cdb: current_configs)
        {
          for (database& db: cdb.dependency_configs ())
          {
            if (find (dep_dbs.begin (), dep_dbs.end (), db) == dep_dbs.end ())
              dep_dbs.push_back (db);
          }
        }

        // Temporarily add the replacement prerequisites to the repointed
        // dependent prerequisites sets and persist the changes.
        //
        for (auto& rd: rpt_depts)
        {
          database&           db (rd.first.db);
          const package_name& nm (rd.first.name);

          shared_ptr<selected_package> sp (db.load<selected_package> (nm));
          package_prerequisites& prereqs (sp->prerequisites);

          for (const auto& prq: rd.second)
          {
            if (prq.second) // Prerequisite replacement?
            {
              const package_key& p (prq.first);

              // Find the being replaced prerequisite to copy it's information
              // into the replacement.
              //
              auto i (find_if (prereqs.begin (), prereqs.end (),
                               [&p] (const auto& pr)
                               {
                                 return pr.first.object_id () == p.name;
                               }));

              assert (i != prereqs.end ());

              auto j (prereqs.emplace (
                        lazy_shared_ptr<selected_package> (p.db.get (),
                                                           p.name),
                        i->second));

              // The selected package should only contain the old
              // prerequisites at this time, so adding a replacement should
              // always succeed.
              //
              assert (j.second);
            }
          }

          db.update (sp);
        }

        // Erase the replacements from the repointed dependents prerequisite
        // sets and persist the changes.
        //
        auto restore_repointed_dependents = [&rpt_depts] ()
        {
          for (auto& rd: rpt_depts)
          {
            database&           db (rd.first.db);
            const package_name& nm (rd.first.name);

            shared_ptr<selected_package> sp (db.load<selected_package> (nm));

            for (const auto& prq: rd.second)
            {
              if (prq.second) // Prerequisite replacement?
              {
                const package_key& p (prq.first);

                size_t n (
                  sp->prerequisites.erase (
                    lazy_shared_ptr<selected_package> (p.db.get (), p.name)));

                // The selected package should always contain the prerequisite
                // replacement at this time, so its removal should always
                // succeed.
                //
                assert (n == 1);
              }
            }

            db.update (sp);
          }
        };

        // Pre-enter dependency to keep track of the desired versions and
        // options specified on the command line. In particular, if the
        // version is specified and the dependency is used as part of the
        // plan, then the desired version must be used. We also need it to
        // distinguish user-driven dependency up/down-grades from the
        // dependent-driven ones, not to warn/refuse.
        //
        // Also, if a dependency package already has selected package that
        // is held, then we need to unhold it.
        //
        auto enter = [&pkgs, &cmd_line] (database& db,
                                         const dependency_package& p)
        {
          // Note that we don't set the upgrade and deorphan flags based on
          // the --upgrade, --patch, and --deorphan options since an option
          // presense doesn't necessarily means that the respective flag needs
          // to be set (the package may not be selected, may not be patchable
          // and/or an orphan, etc). The proper flags will be provided by
          // evaluate_dependency() if/when any upgrade/deorphan recommendation
          // is given.
          //
          build_package bp {
            nullopt,                    // Action.
            db,
            nullptr,                    // Selected package.
            nullptr,                    // Available package/repo fragment.
            nullptr,
            nullopt,                    // Dependencies.
            nullopt,                    // Dependencies alternatives.
            nullopt,                    // Package skeleton.
            nullopt,                    // Postponed dependency alternatives.
            false,                      // Recursive collection.
            false,                      // Hold package.
            p.hold_version,
            {},                         // Constraints.
            p.system,
            p.keep_out,
            p.disfigure,
            false,                      // Configure-only.
            p.checkout_root,
            p.checkout_purge,
            p.config_vars,
            nullopt,                    // Upgrade.
            false,                      // Deorphan.
            {cmd_line},                 // Required by (command line).
            false,                      // Required by dependents.
            0};                         // State flags.

          if (p.constraint)
            bp.constraints.emplace_back (*p.constraint,
                                         cmd_line.db,
                                         cmd_line.name.string ());

          pkgs.enter (p.name, move (bp));
        };

        // Add the system dependency to the database's system repository and
        // pre-enter it to the build package map.
        //
        auto enter_system_dependency = [&add_system_package, &enter]
          (database& db, const dependency_package& p)
        {
          // The system package may only have an exact/wildcard version
          // specified.
          //
          add_system_package (&db,
                              p.name,
                              p.constraint,
                              p.system_status,
                              nullptr /* stubs */);
          enter (db, p);
        };

        // Private configurations that were created during collection of the
        // package builds. The list contains the private configuration paths,
        // relative to the containing configuration directories (.bpkg/host/,
        // etc), together with the containing configuration databases.
        //
        // Note that the private configurations are linked to their parent
        // configurations right after being created, so that the subsequent
        // collecting, ordering, and plan execution simulation logic can use
        // them. However, we can not easily commit these changes at some
        // point, since there could also be some other changes made to the
        // database which needs to be rolled back at the end of the refinement
        // iteration.
        //
        // Thus, the plan is to collect configurations where the private
        // configurations were created and, after the transaction is rolled
        // back, re-link these configurations and persist the changes using
        // the new transaction.
        //
        vector<pair<database&, dir_path>> priv_cfgs;

        // Add a newly created private configuration to the private
        // configurations and the dependency databases lists and pre-enter
        // builds of system dependencies with unspecified configuration for
        // this configuration.
        //
        const function<build_packages::add_priv_cfg_function> add_priv_cfg (
          [&priv_cfgs, &dep_dbs, &dep_pkgs, &enter_system_dependency]
          (database& pdb, dir_path&& cfg)
          {
            database& db (pdb.find_attached (pdb.config / cfg,
                                             false /* self */));

            priv_cfgs.emplace_back (pdb, move (cfg));

            dep_dbs.push_back (db);

            for (const dependency_package& p: dep_pkgs)
            {
              if (p.db == nullptr)
                enter_system_dependency (db, p);
            }
          });

        postponed_packages              postponed_repo;
        postponed_packages              postponed_alts;
        postponed_packages              postponed_recs;
        postponed_existing_dependencies postponed_edeps;
        postponed_configurations        postponed_cfgs;
        strings                         postponed_cfgs_history;
        unsatisfied_dependents          unsatisfied_depts;

        try
        {
          if (scratch)
          {
            pkgs.clear ();

            if (scratch_exe)
            {
              replaced_vers.clear ();
              postponed_deps.clear ();
              unacceptable_alts.clear ();

              scratch_exe = false;
            }
            else
            {
              assert (scratch_col); // See the scratch definition above.

              // Reset to detect bogus entries.
              //
              for (auto& rv: replaced_vers)
                rv.second.replaced = false;

              for (auto& pd: postponed_deps)
              {
                pd.second.wout_config = false;
                pd.second.with_config = false;
              }

              scratch_col = false;
            }

            // Pre-enter dependencies with specified configurations.
            //
            for (const dependency_package& p: dep_pkgs)
            {
              if (p.db != nullptr)
                enter (*p.db, p);
            }

            // Pre-enter system dependencies with unspecified configuration
            // for all dependency configurations, excluding those which
            // already have this dependency pre-entered.
            //
            for (const dependency_package& p: dep_pkgs)
            {
              if (p.db == nullptr)
              {
                for (database& db: dep_dbs)
                {
                  if (!pkgs.entered_build (db, p.name))
                    enter_system_dependency (db, p);
                }
              }
            }

            // Pre-collect user selection to make sure dependency-forced
            // up/down-grades are handled properly (i.e., the order in which we
            // specify packages on the command line does not matter).
            //
            for (const build_package& p: hold_pkgs)
              pkgs.collect_build (
                o, p, replaced_vers, postponed_cfgs, unsatisfied_depts);

            // Collect all the prerequisites of the user selection.
            //
            // Note that some of the user-selected packages can well be
            // dependencies whose recursive processing should be postponed.
            //
            for (const build_package& p: hold_pkgs)
            {
              package_key pk (p.db, p.name ());

              auto i (postponed_deps.find (pk));

              if (i != postponed_deps.end ())
              {
                // Even though the user selection may have a configuration, we
                // treat it as a dependent without any configuration because
                // it is non-negotiable, known at the outset, and thus cannot
                // be a reason to postpone anything.
                //
                i->second.wout_config = true;

                l5 ([&]{trace << "dep-postpone user-specified " << pk;});
              }
              else
              {
                const postponed_configuration* pcfg (
                  postponed_cfgs.find_dependency (pk));

                if (pcfg != nullptr)
                {
                  l5 ([&]{trace << "dep-postpone user-specified " << pk
                                << " since already in cluster " << *pcfg;});
                }
                else
                {
                  pkgs.collect_build_prerequisites (
                    o,
                    p.db,
                    p.name (),
                    find_prereq_database,
                    add_priv_cfg,
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
              }
            }

            // Note that we need to collect unheld after prerequisites, not to
            // overwrite the pre-entered entries before they are used to
            // provide additional constraints for the collected prerequisites.
            //
            for (const dependency_package& p: dep_pkgs)
            {
              auto unhold = [&p, &pkgs] (database& db)
              {
                shared_ptr<selected_package> sp (
                  p.db != nullptr
                  ? p.selected
                  : db.find<selected_package> (p.name));

                if (sp != nullptr && sp->hold_package)
                  pkgs.collect_unhold (db, sp);
              };

              if (p.db != nullptr)
              {
                unhold (*p.db);
              }
              else
              {
                for (database& db: dep_dbs)
                  unhold (db);
              }
            }

            // Collect dependents whose dependencies need to be repointed to
            // packages from different configurations.
            //
            pkgs.collect_repointed_dependents (o,
                                               rpt_depts,
                                               replaced_vers,
                                               postponed_repo,
                                               postponed_alts,
                                               postponed_recs,
                                               postponed_edeps,
                                               postponed_deps,
                                               postponed_cfgs,
                                               unacceptable_alts,
                                               unsatisfied_depts,
                                               find_prereq_database,
                                               add_priv_cfg);
          }
          else
            pkgs.clear_order (); // Only clear the ordered list.

          // Add to the plan dependencies to up/down-grade/drop that were
          // discovered on the previous iterations.
          //
          // Note: this loop takes care of both the from-scratch and
          // refinement cases.
          //
          for (const dep& d: deps)
          {
            database& ddb (d.db);

            if (d.available == nullptr)
            {
              pkgs.collect_drop (o,
                                 ddb,
                                 ddb.load<selected_package> (d.name),
                                 replaced_vers);
            }
            else
            {
              shared_ptr<selected_package> sp (
                ddb.find<selected_package> (d.name));

              // We will keep the output directory only if the external package
              // is replaced with an external one (see above for details).
              //
              bool keep_out (o.keep_out () && sp->external ());

              // Marking upgraded dependencies as "required by command line"
              // may seem redundant as they should already be pre-entered as
              // such (see above). But remember dependencies upgraded with
              // -i|-r? Note that the required_by data member should never be
              // empty, as it is used in prompts/diagnostics.
              //
              build_package p {
                build_package::build,
                ddb,
                move (sp),
                d.available,
                d.repository_fragment,
                nullopt,                 // Dependencies.
                nullopt,                 // Dependencies alternatives.
                nullopt,                 // Package skeleton.
                nullopt,                 // Postponed dependency alternatives.
                false,                   // Recursive collection.
                nullopt,                 // Hold package.
                nullopt,                 // Hold version.
                {},                      // Constraints.
                d.system,
                keep_out,
                o.disfigure (),
                false,                   // Configure-only.
                nullopt,                 // Checkout root.
                false,                   // Checkout purge.
                strings (),              // Configuration variables.
                d.upgrade,
                d.deorphan,
                {cmd_line},              // Required by (command line).
                false,                   // Required by dependents.
                (d.existing || d.deorphan
                 ? build_package::build_replace
                 : uint16_t (0))};

              package_key pk {ddb, d.name};

              // Similar to the user-selected packages, collect non-
              // recursively the dependencies for which recursive collection
              // is postponed (see above for details).
              //
              auto i (postponed_deps.find (pk));
              if (i != postponed_deps.end ())
              {
                i->second.wout_config = true;

                // Note: not recursive.
                //
                pkgs.collect_build (
                  o, move (p), replaced_vers, postponed_cfgs, unsatisfied_depts);

                l5 ([&]{trace << "dep-postpone user-specified dependency "
                              << pk;});
              }
              else
              {
                const postponed_configuration* pcfg (
                  postponed_cfgs.find_dependency (pk));

                if (pcfg != nullptr)
                {
                  // Note: not recursive.
                  //
                  pkgs.collect_build (o,
                                      move (p),
                                      replaced_vers,
                                      postponed_cfgs,
                                      unsatisfied_depts);

                  l5 ([&]{trace << "dep-postpone user-specified dependency "
                                << pk << " since already in cluster "
                                << *pcfg;});
                }
                else
                {
                  build_package_refs dep_chain;

                  // Note: recursive.
                  //
                  pkgs.collect_build (o,
                                      move (p),
                                      replaced_vers,
                                      postponed_cfgs,
                                      unsatisfied_depts,
                                      &dep_chain,
                                      find_prereq_database,
                                      add_priv_cfg,
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

          // Handle the (combined) postponed collection.
          //
          if (find_if (postponed_recs.begin (), postponed_recs.end (),
                       [] (const build_package* p)
                       {
                         // Note that we check for the dependencies presence
                         // rather than for the recursive_collection flag
                         // (see collect_build_postponed() for details).
                         //
                         return !p->dependencies;
                       }) != postponed_recs.end () ||
              !postponed_repo.empty ()             ||
              !postponed_alts.empty ()             ||
              postponed_deps.has_bogus ()          ||
              !postponed_cfgs.empty ())
            pkgs.collect_build_postponed (o,
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
                                          find_prereq_database,
                                          rpt_depts,
                                          add_priv_cfg);

          // Erase the bogus replacements and re-collect from scratch, if any
          // (see replaced_versions for details).
          //
          replaced_vers.cancel_bogus (trace, true /* scratch */);
        }
        catch (const scratch_collection& e)
        {
          // Re-collect from scratch (but keep deps).
          //
          scratch_col = true;

          l5 ([&]{trace << "collection failed due to " << e.description
                        << (e.package != nullptr
                            ? " (" + e.package->string () + ')'
                            : empty_string)
                        << ", retry from scratch";});

          // Erase the package version replacements that we didn't apply
          // during the current (re-)collection iteration since the dependents
          // demanding this version are not collected anymore.
          //
          replaced_vers.cancel_bogus (trace, false /* scratch */);

          restore_repointed_dependents ();

          // Commit linking of private configurations that were potentially
          // created during the collection of the package builds with their
          // parent configurations.
          //
          t.commit ();

          continue;
        }

        set<package_key> depts (
          pkgs.collect_dependents (rpt_depts, unsatisfied_depts));

        // Now that we have collected all the package versions that we need to
        // build, arrange them in the "dependency order", that is, with every
        // package on the list only possibly depending on the ones after
        // it. Iterate over the names we have collected on the previous step
        // in reverse so that when we iterate over the packages (also in
        // reverse), things will be built as close as possible to the order
        // specified by the user (it may still get altered if there are
        // dependencies between the specified packages).
        //
        // The order of dependency upgrades/downgrades/drops is not really
        // deterministic. We, however, do upgrades/downgrades before hold_pkgs
        // so that they appear (e.g., on the plan) after the packages being
        // built to hold. We handle drops last, though, so that the unused
        // packages are likely get purged before the package fetches, so that
        // the disk space they occupy can be reused.
        //
        for (const dep& d: deps)
        {
          if (d.available != nullptr)
            pkgs.order (d.db,
                        d.name,
                        find_prereq_database,
                        false /* reorder */);
        }

        for (const build_package& p: reverse_iterate (hold_pkgs))
          pkgs.order (p.db, p.name (), find_prereq_database);

        for (const auto& rd: rpt_depts)
          pkgs.order (rd.first.db,
                      rd.first.name,
                      find_prereq_database,
                      false /* reorder */);

        // Order the existing dependents which have participated in
        // negotiation of the configuration of their dependencies.
        //
        for (const postponed_configuration& cfg: postponed_cfgs)
        {
          for (const auto& d: cfg.dependents)
          {
            if (d.second.existing)
            {
              const package_key& p (d.first);
              pkgs.order (p.db, p.name, find_prereq_database);
            }
          }
        }

        // Order the existing dependents whose dependencies are being
        // up/down-graded or reconfigured.
        //
        for (const package_key& p: depts)
          pkgs.order (p.db, p.name, find_prereq_database, false /* reorder */);

        // Order the re-collected packages (deviated dependents, etc).
        //
        for (build_package* p: postponed_recs)
        {
          assert (p->recursive_collection);

          pkgs.order (p->db, p->name (), find_prereq_database);
        }

        // Make sure all the packages that we need to unhold are on the list.
        //
        for (const dependency_package& p: dep_pkgs)
        {
          auto order_unheld = [&p, &pkgs, &find_prereq_database] (database& db)
          {
            shared_ptr<selected_package> sp (
              p.db != nullptr
              ? p.selected
              : db.find<selected_package> (p.name));

            if (sp != nullptr && sp->hold_package)
              pkgs.order (db,
                          p.name,
                          find_prereq_database,
                          false /* reorder */);
          };

          if (p.db != nullptr)
          {
            order_unheld (*p.db);
          }
          else
          {
            for (database& db: dep_dbs)
              order_unheld (db);
          }
        }

        // And, finally, order the package drops.
        //
        for (const dep& d: deps)
        {
          if (d.available == nullptr)
            pkgs.order (d.db,
                        d.name,
                        find_prereq_database,
                        false /* reorder */);
        }

        // Make sure all the postponed dependencies of existing dependents
        // have been collected and fail if that's not the case.
        //
        for (const auto& pd: postponed_edeps)
        {
          const build_package* p (pkgs.entered_build (pd.first));
          assert (p != nullptr && p->available != nullptr);

          if (!p->recursive_collection)
          {
            // Feels like this shouldn't happen but who knows.
            //
            diag_record dr (fail);
            dr << "package " << p->available_name_version_db () << " is not "
               << "built due to its configured dependents deviation in "
               << "dependency resolution" <<
              info << "deviated dependents:";

            for (const package_key& d: pd.second)
              dr << ' ' << d;

            dr << info << "please report in "
               << "https://github.com/build2/build2/issues/302";
          }
        }

#ifndef NDEBUG
        pkgs.verify_ordering ();
#endif
        // Now, as we are done with package builds collecting/ordering, erase
        // the replacements from the repointed dependents prerequisite sets
        // and persist the changes.
        //
        restore_repointed_dependents ();

        // We are about to execute the plan on the database (but not on the
        // filesystem / actual packages). Save the session state for the
        // selected_package objects so that we can restore it later (see
        // below for details).
        //
        using selected_packages = session::object_map<selected_package>;
        auto sp_session = [] (const auto& tm) -> selected_packages*
        {
          auto i (tm.find (&typeid (selected_package)));
          return (i != tm.end ()
                  ? &static_cast<selected_packages&> (*i->second)
                  : nullptr);
        };

        map<const odb::database*, selected_packages> old_sp;

        for (const auto& dps: ses.map ())
        {
          if (const selected_packages* sps = sp_session (dps.second))
            old_sp.emplace (dps.first, *sps);
        }

        // Note that we need to perform the execution on the copies of the
        // build/drop_package objects to preserve the original ones. The
        // selected_package objects will still be changed so we will reload
        // them afterwards (see below).
        //
        // After the plan execution simulation, save the packages being built
        // (selected non-system packages) for the subsequent dependency
        // hierarchies verification.
        //
        bool changed;
        vector<pair<database&, shared_ptr<selected_package>>> build_pkgs;
        {
          vector<build_package> tmp (pkgs.begin (), pkgs.end ());
          build_package_list bl (tmp.begin (), tmp.end ());

          changed = execute_plan (o,
                                  bl,
                                  &unsatisfied_depts,
                                  find_prereq_database);

          if (changed)
          {
            for (build_package& p: bl)
            {
              shared_ptr<selected_package>& sp (p.selected);

              if (sp != nullptr)
              {
                if (!sp->system ())
                  build_pkgs.emplace_back (p.db, move (sp));
              }
              else
                assert (p.action && *p.action == build_package::drop);
            }
          }
        }

        // Return nullopt if no changes to the dependency are necessary. This
        // value covers both the "no change is required" and the "no
        // recommendation available" cases.
        //
        auto eval_dep = [&dep_pkgs,
                         &rec_pkgs,
                         &o,
                         &existing_deps,
                         &deorphaned_deps,
                         &pkgs,
                         cache = upgrade_dependencies_cache {}] (
                         database& db,
                         const shared_ptr<selected_package>& sp,
                         bool ignore_unsatisfiable = true) mutable
          -> optional<evaluate_result>
        {
          optional<evaluate_result> r;

          // See if there is an optional dependency upgrade recommendation.
          //
          if (!sp->hold_package)
            r = evaluate_dependency (o,
                                     db,
                                     sp,
                                     dep_pkgs,
                                     o.no_move (),
                                     existing_deps,
                                     deorphaned_deps,
                                     pkgs,
                                     ignore_unsatisfiable);

          // If none, then see for the recursive dependency upgrade
          // recommendation.
          //
          // Let's skip upgrading system packages as they are, probably,
          // configured as such for a reason.
          //
          if (!r && !sp->system () && !rec_pkgs.empty ())
            r = evaluate_recursive (o,
                                    db,
                                    sp,
                                    rec_pkgs,
                                    existing_deps,
                                    deorphaned_deps,
                                    pkgs,
                                    ignore_unsatisfiable,
                                    cache);

          // Translate the "no change" result to nullopt.
          //
          return r && r->available == nullptr && !r->unused ? nullopt : r;
        };

        // The empty version means that the package must be dropped.
        //
        const version ev;
        auto target_version = [&ev]
                              (database& db,
                               const shared_ptr<available_package>& ap,
                               bool sys) -> const version&
        {
          if (ap == nullptr)
            return ev;

          if (sys)
          {
            assert (ap->system_version (db) != nullptr);
            return *ap->system_version (db);
          }

          return ap->version;
        };

        // Verify that none of the previously-made upgrade/downgrade/drop
        // decisions have changed.
        //
        for (auto i (deps.begin ()); i != deps.end (); )
        {
          bool s (false);

          database& db (i->db);
          const package_name& nm (i->name);

          // Here we scratch if evaluate changed its mind or if the resulting
          // version doesn't match what we expect it to be.
          //
          if (auto sp = db.find<selected_package> (nm))
          {
            const version& dv (target_version (db, i->available, i->system));

            if (optional<evaluate_result> r = eval_dep (db, sp))
              s = dv != target_version (db, r->available, r->system) ||
                  i->system != r->system;
            else
              s = dv != sp->version || i->system != sp->system ();
          }
          else
            s = i->available != nullptr;

          if (s)
          {
            scratch_exe = true; // Rebuild the plan from scratch.

            package_key pk (db, nm);

            auto j (find (existing_deps.begin (), existing_deps.end (), pk));
            if (j != existing_deps.end ())
              existing_deps.erase (j);

            deorphaned_deps.erase (pk);

            i = deps.erase (i);
          }
          else
            ++i;
        }

        if (scratch_exe)
          l5 ([&]{trace << "one of dependency evaluation decisions has "
                        << "changed, re-collecting from scratch";});

        // If the execute_plan() call was noop, there are no user expectations
        // regarding any dependency, and no upgrade is requested, then the
        // only possible refinement outcome can be recommendations to drop
        // unused dependencies (that the user has refused to drop on the
        // previous build or drop command run). Thus, if the --keep-unused|-K
        // or --no-refinement option is also specified, then we omit the
        // need_refinement() call altogether and assume that no refinement is
        // required.
        //
        if (!changed && dep_pkgs.empty () && rec_pkgs.empty ())
        {
          assert (!scratch_exe); // No reason to change any previous decision.

          if (o.keep_unused () || o.no_refinement ())
            refine = false;
        }

        if (!scratch_exe && refine)
        {
          // First, we check if the refinement is required, ignoring the
          // unsatisfiable dependency version constraints. If we end up
          // refining the execution plan, such dependencies might be dropped,
          // and then there will be nothing to complain about. When no more
          // refinements are necessary we will run the diagnostics check, to
          // make sure that the unsatisfiable dependency, if left, is
          // reported.
          //
          auto need_refinement = [&eval_dep,
                                  &deps,
                                  &rec_pkgs,
                                  &dep_dbs,
                                  &existing_deps,
                                  &deorphaned_deps,
                                  &o] (bool diag = false) -> bool
          {
            // Examine the new dependency set for any up/down-grade/drops.
            //
            bool r (false); // Presumably no more refinements are necessary.

            using query = query<selected_package>;

            query q (query::state == "configured");

            if (rec_pkgs.empty ())
              q = q && !query::hold_package;

            // It seems right to only evaluate dependencies in the explicitly
            // linked configurations, recursively. Indeed, we shouldn't be
            // up/down-grading or dropping packages in configurations that
            // only contain dependents, some of which we may only reconfigure.
            //
            for (database& db: dep_dbs)
            {
              for (shared_ptr<selected_package> sp:
                     pointer_result (db.query<selected_package> (q)))
              {
                if (optional<evaluate_result> er = eval_dep (db, sp, !diag))
                {
                  // Skip unused if we were instructed to keep them.
                  //
                  if (o.keep_unused () && er->available == nullptr)
                    continue;

                  if (!diag)
                  {
                    deps.push_back (dep {er->db,
                                         sp->name,
                                         move (er->available),
                                         move (er->repository_fragment),
                                         er->system,
                                         er->existing,
                                         er->upgrade,
                                         er->orphan.has_value ()});

                    if (er->existing)
                      existing_deps.emplace_back (er->db, sp->name);

                    if (er->orphan)
                    {
                      deorphaned_deps[package_key (er->db, sp->name)] =
                        move (*er->orphan);
                    }
                  }

                  r = true;
                }
              }
            }

            return r;
          };

          refine = need_refinement ();

          // If no further refinement is necessary, then perform the
          // diagnostics run. Otherwise, if any dependency configuration
          // negotiation has been performed during the current plan refinement
          // iteration, then rebuild the plan from scratch (see above for
          // details). Also rebuild it from from scratch if any unsatisfied
          // dependents have been ignored, since their unsatisfied constraints
          // are now added to the dependencies' build_package::constraints
          // lists.
          //
          if (!refine)
            need_refinement (true /* diag */);
          else if (!postponed_cfgs.empty () || !unsatisfied_depts.empty ())
            scratch_exe = true;
        }

        // Note that we prevent building multiple instances of the same
        // package dependency in different configurations (of the same type)
        // while creating the build plan. However, we may potentially end up
        // with the same dependency in multiple configurations since we do not
        // descend into prerequisites of already configured packages which
        // require no up/downgrade.
        //
        // To prevent this, we additionally verify that none of the dependency
        // hierarchies of the packages being built contains the same runtime
        // dependency, built in multiple configurations.
        //
        // Note that we also fail for a system dependency configured in
        // multiple configurations, since these configurations can potentially
        // be configured differently and so these system packages can refer to
        // different targets.
        //
        if (changed && !refine)
        {
          // Verify the specified package dependency hierarchy and return the
          // set of packages plus their runtime dependencies, including
          // indirect ones. Fail if a dependency cycle is detected.
          //
          // Also add the result into the `package_prereqs` map, to use it as
          // a cache and for subsequent additional dependency verification.
          //
          // Note that all the encountered dependency sub-hierarchies that
          // reside in configurations of different types (or beneath them) are
          // also verified but not included into the resulting set.
          //
          using prerequisites = set<lazy_shared_ptr<selected_package>,
                                    compare_lazy_ptr_id>;

          map<package_key, prerequisites> package_prereqs;
          small_vector<config_selected_package, 16> chain;

          auto verify_dependencies = [&package_prereqs, &chain]
                                     (database& db,
                                      shared_ptr<selected_package> sp,
                                      const auto& verify_dependencies)
               -> const prerequisites&
          {
            // Return the cached value, if present.
            //
            package_key pk {db, sp->name};
            {
              auto i (package_prereqs.find (pk));

              if (i != package_prereqs.end ())
                return i->second;
            }

            // Make sure there is no dependency cycle.
            //
            config_selected_package csp {db, sp};
            {
              auto i (find (chain.begin (), chain.end (), csp));

              if (i != chain.end ())
              {
                diag_record dr (fail);
                dr << "dependency cycle detected involving package " << *sp
                   << db;

                // Note: push_back() can invalidate the iterator.
                //
                size_t j (i - chain.begin ());

                for (chain.push_back (csp); j != chain.size () - 1; ++j)
                  dr << info << *chain[j].package << chain[j].db
                             << " depends on "
                             << *chain[j + 1].package << chain[j + 1].db;
              }
            }

            chain.push_back (csp);

            // Verify all prerequisites, but only collect those corresponding
            // to the runtime dependencies.
            //
            // Indeed, we don't care if a linked host configuration contains a
            // configured package that we also have configured in our target
            // configuration. It's also fine if some of our runtime
            // dependencies from different configurations build-time depend on
            // the same package (of potentially different versions) configured
            // in different host configurations.
            //
            // Note, however, that we cannot easily determine if the
            // prerequisite corresponds to the runtime or build-time
            // dependency, since we don't store this information for
            // prerequisites. The current implementation relies on the fact
            // that the build-time dependency configuration type (host or
            // build2) differs from the dependent configuration type (target
            // is a common case) and doesn't work well, for example, for the
            // self-hosted configurations. For them it can fail erroneously.
            // We can potentially fix that by additionally storing the
            // build-time flag for a prerequisite. However, let's first see if
            // it ever becomes a problem.
            //
            prerequisites r;
            const package_prerequisites& prereqs (sp->prerequisites);

            for (const auto& prereq: prereqs)
            {
              const lazy_shared_ptr<selected_package>& p (prereq.first);
              database& pdb (p.database ());

              // Validate prerequisite sub-hierarchy also in configuration of
              // different type but do not collect it.
              //
              const prerequisites& ps (
                verify_dependencies (pdb, p.load (), verify_dependencies));

              if (pdb.type != db.type)
                continue;

              // Collect prerequisite sub-hierarchy, checking that none of the
              // packages are already collected.
              //
              for (const lazy_shared_ptr<selected_package>& p: ps)
              {
                // Note: compare_id_lazy_ptr only considers package names.
                //
                auto i (r.find (p));

                if (i != r.end ())
                {
                  database& db1 (p.database ());
                  database& db2 (i->database ());

                  if (db1 != db2)
                  {
                    bool indirect (prereqs.find (p) == prereqs.end ());

                    fail << "package " << p.object_id ()
                         << (indirect ? " indirectly" : "") << " required by "
                         << *sp << db << " is configured in multiple "
                         << "configurations" <<
                      info << *p.load () << db1 <<
                      info << *i->load () << db2;
                  }
                }
                else
                  r.insert (p);
              }
            }

            chain.pop_back ();

            // Collect the dependent package itself.
            //
            r.insert (lazy_shared_ptr<selected_package> (db, move (sp)));

            // Cache the resulting package prerequisites set and return a
            // reference to it.
            //
            auto j (package_prereqs.emplace (move (pk), move (r)));
            assert (j.second); // A package cannot depend on itself.

            return j.first->second;
          };

          for (auto& p: build_pkgs)
            verify_dependencies (p.first,
                                 move (p.second),
                                 verify_dependencies);

          // Now, verify that none of the build2 modules may simultaneously be
          // built in multiple configurations which belong to the same linked
          // configuration cluster.
          //
          // For that we use the `package_prereqs` map: its key set refers to
          // all the packages potentially involved into the build (explicitly
          // or implicitly).
          //
          {
            // List of module packages together with the linked configuration
            // clusters they belong to.
            //
            vector<pair<package_key, linked_databases>> build2_mods;

            for (const auto& pp: package_prereqs)
            {
              const package_key& pk (pp.first);

              // Skip packages other than the build2 modules.
              //
              if (!build2_module (pk.name))
                continue;

              // Skip build2 modules configured as system.
              //
              {
                shared_ptr<selected_package> sp (
                  pk.db.get ().find<selected_package> (pk.name));

                assert (sp != nullptr);

                if (sp->system ())
                  continue;
              }

              // Make sure the module's database doesn't belong to any other
              // cluster this module is also configured in.
              //
              for (const auto& m: build2_mods)
              {
                if (m.first.name != pk.name)
                  continue;

                // The `package_prereqs` map can only contain the same package
                // twice if databases differ.
                //
                assert (m.first.db != pk.db);

                const linked_databases& lcc (m.second);

                if (find (lcc.begin (), lcc.end (), pk.db) != lcc.end ())
                {
                  fail << "building build system module " << pk.name
                       << " in multiple configurations" <<
                    info << m.first.db.get ().config_orig <<
                    info << pk.db.get ().config_orig;
                }
              }

              // Add the module and its cluster to the list.
              //
              build2_mods.emplace_back (pk, pk.db.get ().cluster_configs ());
            }
          }
        }

        // Rollback the changes to the database and reload the changed
        // selected_package objects.
        //
        t.rollback ();
        {
          transaction t (mdb);

          // First reload all the selected_package object that could have been
          // modified (conceptually, we should only modify what's on the
          // plan). And in case of drop the object is removed from the session
          // so we need to bring it back.
          //
          // Make sure that selected packages are only owned by the session
          // and the build package (pkgs) and the dependency (dep_pkgs) lists.
          //
          build_pkgs.clear ();

          // Note: we use the original pkgs list since the executed ones may
          // contain newly created (but now gone) selected_package objects.
          //
          for (build_package& p: pkgs)
          {
            assert (p.action);

            database& pdb (p.db);

            if (*p.action == build_package::drop)
            {
              assert (p.selected != nullptr);

              ses.cache_insert<selected_package> (
                pdb, p.selected->name, p.selected);
            }

            if (p.selected != nullptr)
              pdb.reload (*p.selected);
          }

          // Now remove all the newly created selected_package objects from
          // the session. The tricky part is to distinguish newly created ones
          // from newly loaded (and potentially cached).
          //
          for (bool rescan (true); rescan; )
          {
            rescan = false;

            for (const auto& dps: ses.map ())
            {
              if (selected_packages* sps = sp_session (dps.second))
              {
                auto j (old_sp.find (dps.first)); // Find the database.

                // Note that if a database has been introduced only during
                // simulation, then we could just clear all its selected
                // packages in one shot. Let's however, be cautious and remove
                // them iteratively to make sure that none of them are left at
                // the end (no more rescan is necessary). If any of them is
                // left, then that would mean that is is referenced from
                // somewhere besides the session object, which would be a bug.
                //
                if (j == old_sp.end ())
                {
                  if (!sps->empty ())
                  {
                    for (auto i (sps->begin ()); i != sps->end (); )
                    {
                      if (i->second.use_count () == 1)
                      {
                        // This might cause another object's use count to drop.
                        //
                        i = sps->erase (i);
                        rescan = true;
                      }
                      else
                        ++i;
                    }
                  }

                  continue;
                }

                const selected_packages& osp (j->second);

                for (auto i (sps->begin ()); i != sps->end (); )
                {
                  bool erased (false);
                  auto j (osp.find (i->first));

                  if (j == osp.end ())
                  {
                    if (i->second.use_count () == 1)
                    {
                      // This might cause another object's use count to drop.
                      //
                      i = sps->erase (i);
                      erased = true;
                      rescan = true;
                    }
                  }
                  // It may also happen that the object was erased from the
                  // database and then recreated. In this case we restore the
                  // pointer that is stored in the session.
                  //
                  else if (i->second != j->second)
                  {
                    // This might cause another object's use count to drop.
                    //
                    i->second = j->second;
                    rescan = true;
                  }

                  if (!erased)
                    ++i;
                }
              }
            }

            // Verify that all the selected packages of the newly introduced
            // during simulation databases are erased (see above for the
            // verification reasoning).
            //
            if (!rescan)
            {
              // Return true if the specified package is loaded as a
              // prerequisite of some dependent package, cached in the
              // session, and contained in a different database. In this case
              // unload this package from all such dependents.
              //
              auto check_unload_prereq = [&ses, &sp_session]
                (const shared_ptr<selected_package>& sp,
                 const odb::database* db)
              {
                bool r (false);

                for (const auto& dps: ses.map ())
                {
                  // Skip dependents from the same database.
                  //
                  if (dps.first == db)
                    continue;

                  if (const selected_packages* sps = sp_session (dps.second))
                  {
                    for (const auto& p: *sps)
                    {
                      for (auto& pr: p.second->prerequisites)
                      {
                        const lazy_shared_ptr<selected_package>& lp (pr.first);

                        if (lp.loaded () && lp.get_eager () == sp)
                        {
                          lp.unload ();
                          r = true;
                        }
                      }
                    }
                  }
                }

                return r;
              };

              for (const auto& dps: ses.map ())
              {
                if (const selected_packages* sps = sp_session (dps.second))
                {
                  if (old_sp.find (dps.first) == old_sp.end ())
                  {
                    // Note that the valid reason for these packages to still
                    // be present in the session is that some of them may be
                    // referenced as prerequisites by some dependent packages
                    // from other databases and reference the remaining
                    // packages. For example:
                    //
                    // new session: A (X, 2) -> B (X, 2) -> C (Y, 2) -> D (Y, 2)
                    // old session: A
                    //
                    // Here C and D are the packages in question, package A is
                    // present in both sessions, X and Y are the databases,
                    // the numbers are the package reference counts, and the
                    // arrows denote the loaded prerequisite lazy pointers.
                    //
                    // Let's verify that's the only situation by unloading
                    // these packages from such dependent prerequisites and
                    // rescanning.
                    //
                    if (!sps->empty ())
                    {
                      for (const auto& p: *sps)
                      {
                        if (check_unload_prereq (p.second, dps.first))
                          rescan = true;
                      }

                      // If we didn't unload any of these packages, then we
                      // consider this a bug.
                      //
                      assert (rescan);
                    }
                  }
                }
              }
            }
          }

          // Re-link the private configurations that were created during the
          // collection of the package builds with their parent
          // configurations. Note that these links were lost on the previous
          // transaction rollback.
          //
          for (const pair<database&, dir_path>& pc: priv_cfgs)
            cfg_link (pc.first,
                      pc.first.config / pc.second,
                      true    /* relative */,
                      nullopt /* name */,
                      true    /* sys_rep */);

          t.commit ();
        }

        if (!refine)
        {
          // Cleanup the package build collecting state, preparing for the
          // re-collection from the very beginning.
          //
          auto prepare_recollect = [&refine,
                                    &scratch_exe,
                                    &deps,
                                    &existing_deps,
                                    &deorphaned_deps] ()
          {
            refine = true;
            scratch_exe = true;

            deps.clear ();
            existing_deps.clear ();
            deorphaned_deps.clear ();
          };

          // Issue diagnostics and fail if any existing dependents are not
          // satisfied with their dependencies.
          //
          // But first, try to resolve the first encountered unsatisfied
          // constraint by replacing the collected unsatisfactory dependency
          // or some of its dependents with some other available package
          // version. This version, while not being the best possible choice,
          // must be satisfactory for all its new and existing dependents. If
          // succeed, punch the replacement version into the command line and
          // recollect from the very beginning (see unsatisfied_dependents for
          // details).
          //
          if (!unsatisfied_depts.empty ())
          {
            if (!cmdline_refine_index) // Not command line adjustments refinement?
            {
              const unsatisfied_dependent& dpt (unsatisfied_depts.front ());

              assert (!dpt.ignored_constraints.empty ());

              const ignored_constraint& ic (dpt.ignored_constraints.front ());

              const build_package* p (pkgs.entered_build (ic.dependency));
              assert (p != nullptr); // The dependency must be collected.

              l5 ([&]{trace << "try to replace unsatisfactory dependency "
                            << p->available_name_version_db () << " with some "
                            << "other version";});

              optional<cmdline_adjustment> a;
              vector<package_key> unsatisfied_dpts;
              set<const build_package*> visited_dpts;

              if ((a = try_replace_dependency (o,
                                               *p,
                                               cmdline_allow_downgrade,
                                               pkgs,
                                               hold_pkgs,
                                               dep_pkgs,
                                               cmdline_adjs,
                                               unsatisfied_dpts,
                                               "unsatisfactory dependency")) ||
                  (a = try_replace_dependent (o,
                                              *p,
                                              cmdline_allow_downgrade,
                                              &ic.unsatisfied_constraints,
                                              pkgs,
                                              cmdline_adjs,
                                              unsatisfied_dpts,
                                              hold_pkgs,
                                              dep_pkgs,
                                              visited_dpts))                 ||
                  !cmdline_adjs.empty ())
              {
                if (a)
                {
                  cmdline_adjs.push (move (*a));
                }
                else
                {
                  cmdline_adjustment a (cmdline_adjs.pop ());

                  l5 ([&]{trace << "cannot replace any package, rolling back "
                                << "latest command line adjustment ("
                                << cmdline_adjs.to_string (a) << ')';});
                }

                prepare_recollect ();
              }
              else
              {
                // If we fail to resolve the unsatisfied dependency
                // constraints with the downgrades disallowed, then allow
                // downgrades and retry from the very beginning.
                //
                if (!cmdline_allow_downgrade)
                {
                  l5 ([&]{trace << "cannot resolve unsatisfied dependency "
                                << "constraints, now allowing downgrades";});

                  cmdline_allow_downgrade = true;

                  prepare_recollect ();
                }
                else
                {
                  // Issue the diagnostics and fail.
                  //
                  unsatisfied_depts.diag (pkgs);
                }
              }
            }
            else // We are in the command line adjustments refinement cycle.
            {
              // Since we have failed to collect, then the currently dropped
              // command line adjustment is essential. Thus, push it back to
              // the stack, drop the next one, and retry. If this is the last
              // adjustment in the stack, then we assume that no further
              // refinement is possible and we just recollect, assuming that
              // this recollection will be successful.
              //
              assert (cmdline_refine_adjustment); // Wouldn't be here otherwise.

              l5 ([&]{trace << "attempt to refine command line adjustments by "
                            << "rolling back adjustment "
                            << cmdline_adjs.to_string (
                                 *cmdline_refine_adjustment)
                            << " failed, pushing it back";});

              cmdline_adjs.push (move (*cmdline_refine_adjustment));

              // Index of the being previously dropped adjustment must be
              // valid.
              //
              assert (*cmdline_refine_index != cmdline_adjs.size ());

              if (++(*cmdline_refine_index) != cmdline_adjs.size ())
              {
                cmdline_refine_adjustment = cmdline_adjs.pop (true /* front */);

                l5 ([&]{trace << "continue with command line adjustments "
                              << "refinement cycle by rolling back adjustment "
                              << cmdline_adjs.to_string (
                                   *cmdline_refine_adjustment);});
              }
              else
              {
                cmdline_refine_adjustment = nullopt;

                l5 ([&]{trace << "cannot further refine command line "
                              << "adjustments, performing final collection";});
              }

              prepare_recollect ();
            }
          }
          //
          // If the collection was successful, then see if we still need to
          // perform the command line adjustments refinement.
          //
          else if (cmdline_adjs.tried () &&
                   (!cmdline_refine_index ||
                    *cmdline_refine_index != cmdline_adjs.size ()))
          {
            // If some command line adjustment is currently being dropped,
            // that means that this adjustment is redundant.
            //
            bool initial (!cmdline_refine_index);

            if (!initial)
            {
              assert (cmdline_refine_adjustment);

              l5 ([&]{trace << "command line adjustment "
                            << cmdline_adjs.to_string (
                                 *cmdline_refine_adjustment)
                            << " is redundant, dropping it";});

              cmdline_refine_adjustment = nullopt;
              cmdline_refine_index      = nullopt;
            }

            // We cannot remove all the adjustments during the refinement.
            // Otherwise, we shouldn't be failing in the first place.
            //
            assert (!cmdline_adjs.empty ());

            // If there is just a single adjustment left, then there is
            // nothing to refine anymore.
            //
            if (cmdline_adjs.size () != 1)
            {
              cmdline_refine_adjustment = cmdline_adjs.pop (true /* front */);
              cmdline_refine_index      = 0;

              l5 ([&]{trace << (initial ? "start" : "re-start") << " command "
                            << "line adjustments refinement cycle by rolling "
                            << "back first adjustment ("
                            << cmdline_adjs.to_string (
                                 *cmdline_refine_adjustment)
                            << ')';});

              prepare_recollect ();
            }
          }
        }
      }
    }

    // Print what we are going to do, then ask for the user's confirmation.
    // While at it, detect if we have any dependents that the user may want to
    // update.
    //
    // For the packages being printed also print the configuration specified
    // by the user, dependents, and via the reflect clauses. For that we will
    // use the package skeletons, initializing them if required. Note that for
    // a system package the skeleton may already be initialized during the
    // dependency negotiation process. Also note that the freshly-initialized
    // skeletons will be reused during the plan execution.
    //
    bool update_dependents (false);

    // We need the plan and to ask for the user's confirmation only if some
    // implicit action (such as building prerequisite, reconfiguring dependent
    // package, or installing system/distribution packages) is to be taken or
    // there is a selected package which version must be changed. But if the
    // user explicitly requested it with --plan, then we print it as long as
    // it is not empty.
    //
    string plan;
    sha256 csum;
    bool need_prompt (false);

    if (!o.yes ()           ||
        o.print_only ()     ||
        o.plan_specified () ||
        o.rebuild_checksum_specified ())
    {
      // Map the main system/distribution packages that need to be installed
      // to the system packages which caused their installation (see
      // build_package::system_install() for details).
      //
      using package_names = vector<reference_wrapper<const package_name>>;
      using system_map = map<string, package_names>;

      system_map sys_map;

      // Iterate in the reverse order as we will do for printing the action
      // lines. This way a sys-install action line will be printed right
      // before the bpkg action line of a package which appears first in the
      // sys-install action's 'required by' list.
      //
      for (const build_package& p: reverse_iterate (pkgs))
      {
        if (const system_package_status* s = p.system_install ())
        {
          package_names& ps (sys_map[s->system_name]);

          if (find (ps.begin (), ps.end (), p.name ()) == ps.end ())
            ps.push_back (p.name ());
        }
      }

      // Start the transaction since we may query available packages for
      // skeleton initializations.
      //
      transaction t (mdb);

      bool first (true); // First entry in the plan.

      // Print the bpkg package action lines.
      //
      // Also print the sys-install action lines for system/distribution
      // packages which require installation by the system package manager.
      // Print them before the respective system package action lines, but
      // only once per (main) system/distribution package. For example:
      //
      // sys-install libssl1.1/1.1.1l (required by sys:libssl, sys:libcrypto)
      // configure sys:libssl/1.1.1 (required by foo)
      // configure sys:libcrypto/1.1.1 (required by bar)
      //
      for (auto i (pkgs.rbegin ()); i != pkgs.rend (); )
      {
        build_package& p (*i);
        assert (p.action);

        string act;

        const system_package_status* s;
        system_map::iterator j;

        if ((s = p.system_install ()) != nullptr &&
            (j = sys_map.find (s->system_name)) != sys_map.end ())
        {
          act = "sys-install ";
          act += s->system_name;
          act += '/';
          act += s->system_version;
          act += " (required by ";

          bool first (true);
          for (const package_name& n: j->second)
          {
            if (first)
              first = false;
            else
              act += ", ";

            act += "sys:";
            act += n.string ();
          }

          act += ')';

          need_prompt = true;

          // Make sure that we print this sys-install action just once.
          //
          sys_map.erase (j);

          // Note that we don't increment i in order to re-iterate this pkgs
          // entry.
        }
        else
        {
          ++i;

          database& pdb (p.db);
          const shared_ptr<selected_package>& sp (p.selected);

          if (*p.action == build_package::drop)
          {
            act = "drop " + sp->string (pdb) + " (unused)";
            need_prompt = true;
          }
          else
          {
            // Print configuration variables.
            //
            // The idea here is to only print configuration for those packages
            // for which we call pkg_configure*() in execute_plan().
            //
            package_skeleton* cfg (nullptr);

            string cause;
            if (*p.action == build_package::adjust)
            {
              assert (sp != nullptr && (p.reconfigure () || p.unhold ()));

              // This is a dependent needing reconfiguration.
              //
              // This is an implicit reconfiguration which requires the plan
              // to be printed. Will flag that later when composing the list
              // of prerequisites.
              //
              if (p.reconfigure ())
              {
                act = "reconfigure";
                cause = "dependent of";

                if (!o.configure_only ())
                  update_dependents = true;
              }

              // This is a held package needing unhold.
              //
              if (p.unhold ())
              {
                if (act.empty ())
                  act = "unhold";
                else
                  act += "/unhold";
              }

              act += ' ' + sp->name.string ();

              const string& s (pdb.string);
              if (!s.empty ())
                act += ' ' + s;

              // This is an adjustment and so there is no available package
              // specified for the build package object and thus the skeleton
              // cannot be present.
              //
              assert (p.available == nullptr && !p.skeleton);

              // We shouldn't be printing configurations for plain unholds.
              //
              if (p.reconfigure ())
              {
                // Since there is no available package specified we need to
                // find it (or create a transient one).
                //
                cfg = &p.init_skeleton (o,
                                        true /* load_old_dependent_config */,
                                        find_available (o, pdb, sp));
              }
            }
            else
            {
              assert (p.available != nullptr); // This is a package build.

              bool replace (p.replace ());

              // Even if we already have this package selected, we have to
              // make sure it is configured and updated.
              //
              if (sp == nullptr)
              {
                act = p.system ? "configure" : "new";

                // For a new non-system package the skeleton must already be
                // initialized.
                //
                assert (p.system || p.skeleton.has_value ());

                // Initialize the skeleton if it is not initialized yet.
                //
                cfg = &(p.skeleton ? *p.skeleton : p.init_skeleton (o));
              }
              else if (sp->version == p.available_version ())
              {
                // If this package is already configured and is not part of
                // the user selection (or we are only configuring), then there
                // is nothing we will be explicitly doing with it (it might
                // still get updated indirectly as part of the user selection
                // update).
                //
                if (!p.reconfigure () &&
                    sp->state == package_state::configured &&
                    (!p.user_selection () ||
                     o.configure_only ()  ||
                     p.configure_only ()))
                  continue;

                act = p.system
                  ? "reconfigure"
                  : (p.reconfigure ()
                     ? (o.configure_only () || p.configure_only ()
                        ? (replace ? "replace"        : "reconfigure")
                        : (replace ? "replace/update" : "reconfigure/update"))
                     : "update");

                if (p.reconfigure ())
                {
                  // Initialize the skeleton if it is not initialized yet.
                  //
                  cfg = &(p.skeleton ? *p.skeleton : p.init_skeleton (o));
                }
              }
              else
              {
                act += p.system
                  ? "reconfigure"
                  : (sp->version < p.available_version ()
                     ? (replace ? "replace/upgrade"   : "upgrade")
                     : (replace ? "replace/downgrade" : "downgrade"));

                // For a non-system package up/downgrade the skeleton must
                // already be initialized.
                //
                assert (p.system || p.skeleton.has_value ());

                // Initialize the skeleton if it is not initialized yet.
                //
                cfg = &(p.skeleton ? *p.skeleton : p.init_skeleton (o));

                need_prompt = true;
              }

              if (p.unhold ())
                act += "/unhold";

              act += ' ' + p.available_name_version_db ();
              cause = p.required_by_dependents ? "required by" : "dependent of";

              if (p.configure_only ())
                update_dependents = true;
            }

            // Also list dependents for the newly built user-selected
            // dependencies.
            //
            bool us (p.user_selection ());
            string rb;
            if (!us || (!p.user_selection (hold_pkgs) && sp == nullptr))
            {
              // Note: if we are ever tempted to truncate this, watch out for
              // the --rebuild-checksum functionality which uses this. But
              // then it's not clear this information is actually important:
              // can a dependent-dependency structure change without any of
              // the package versions changing? Doesn't feel like it should.
              //
              for (const package_version_key& pvk: p.required_by)
              {
                // Skip the command-line, etc dependents and don't print the
                // package version (which is not always available; see
                // build_package::required_by for details).
                //
                if (pvk.version) // Is it a real package?
                {
                  rb += (rb.empty () ? " " : ", ") +
                        pvk.string (true /* ignore_version */);
                }
              }

              // If not user-selected, then there should be another (implicit)
              // reason for the action.
              //
              assert (!rb.empty ());
            }

            if (!rb.empty ())
              act += " (" + cause + rb + ')';

            if (cfg != nullptr && !cfg->empty_print ())
            {
              ostringstream os;
              cfg->print_config (os, o.print_only () ? "  " : "    ");
              act += '\n';
              act += os.str ();
            }

            if (!us)
              need_prompt = true;
          }
        }

        if (first)
        {
          // If the plan header is not empty, now is the time to print it.
          //
          if (!o.plan ().empty ())
          {
            if (o.print_only ())
              cout << o.plan () << endl;
            else
              plan += o.plan ();
          }

          first = false;
        }

        if (o.print_only ())
          cout << act << endl;
        else
          // Print indented for better visual separation.
          //
          plan += (plan.empty () ? "  " : "\n  ") + act;

        if (o.rebuild_checksum_specified ())
          csum.append (act);
      }

      t.commit ();
    }

    if (o.rebuild_checksum_specified ())
    {
      cout << csum.string () << endl;

      if (o.rebuild_checksum () == csum.string ())
        return o.noop_exit_specified () ? o.noop_exit () : 0;
    }

    if (o.print_only ())
      return 0;

    if (need_prompt || (o.plan_specified () && !plan.empty ()))
      text << plan;

    // Ask the user if we should continue.
    //
    if (!(o.yes () || !need_prompt || yn_prompt ("continue? [Y/n]", 'y')))
      return 1;

    // Figure out if we also should update dependents.
    //
    if (o.leave_dependent ())
      update_dependents = false;
    else if (o.yes () || o.update_dependent ())
      update_dependents = true;
    else if (update_dependents) // Don't prompt if there aren't any.
      update_dependents = yn_prompt ("update dependent packages? [Y/n]", 'y');

    // Ok, we have "all systems go". The overall action plan is as follows.
    //
    // 1.  sys-install     not installed system/distribution
    // 2.  disfigure       up/down-graded, reconfigured       [left to right]
    // 3.  purge           up/down-graded                     [right to left]
    // 4.a fetch/unpack    new, up/down-graded, replaced
    // 4.b checkout        new, up/down-graded, replaced
    // 5.  configure       all
    // 6.  unhold          unheld
    // 7.  build           user selection                     [right to left]
    //
    // Note that for some actions, e.g., purge or fetch, the order is not
    // really important. We will, however, do it right to left since that
    // is the order closest to that of the user selection.
    //
    // We are also going to combine purge and fetch/unpack|checkout into a
    // single step and use the replace mode so it will become just
    // fetch/unpack|checkout.
    //
    // We also have the dependent packages that we reconfigure because their
    // prerequsites got upgraded/downgraded and that the user may want to in
    // addition update (that update_dependents flag above).
    //
    bool noop (!execute_plan (o,
                              pkgs,
                              nullptr /* simulate */,
                              find_prereq_database));

    if (o.configure_only ())
      return noop && o.noop_exit_specified () ? o.noop_exit () : 0;

    // update
    //
    // Here we want to update all the packages at once, to facilitate
    // parallelism.
    //
    vector<pkg_command_vars> upkgs;

    // First add the user selection.
    //
    // Only update user-selected packages which are specified on the command
    // line as build to hold. Note that the dependency package will be updated
    // implicitly via their dependents, if the latter are updated.
    //
    for (const build_package& p: reverse_iterate (pkgs))
    {
      assert (p.action);

      if (*p.action != build_package::build || p.configure_only ())
        continue;

      database& db (p.db);
      const shared_ptr<selected_package>& sp (p.selected);

      if (!sp->system () && // System package doesn't need update.
          p.user_selection (hold_pkgs))
        upkgs.push_back (pkg_command_vars {db.config_orig,
                                           !multi_config () && db.main (),
                                           sp,
                                           strings () /* vars */,
                                           false /* cwd */});
    }

    // Then add dependents. We do it as a separate step so that they are
    // updated after the user selection.
    //
    if (update_dependents)
    {
      for (const build_package& p: reverse_iterate (pkgs))
      {
        assert (p.action);

        database& db (p.db);

        // Note: don't update the re-evaluated and re-collected dependents
        // unless they are reconfigured.
        //
        if ((*p.action == build_package::adjust && p.reconfigure ()) ||
            (*p.action == build_package::build &&
             ((p.flags & build_package::build_repoint) != 0   ||
              ((p.flags & (build_package::build_reevaluate |
                           build_package::build_recollect)) != 0 &&
               p.reconfigure ()))))
          upkgs.push_back (pkg_command_vars {db.config_orig,
                                             !multi_config () && db.main (),
                                             p.selected,
                                             strings () /* vars */,
                                             false /* cwd */});
      }
    }

    pkg_update (o, o.for_ (), strings (), upkgs);

    if (verb && !o.no_result ())
    {
      for (const pkg_command_vars& pv: upkgs)
        text << "updated " << pv.string ();
    }

    return 0;
  }

  static bool
  execute_plan (const pkg_build_options& o,
                build_package_list& build_pkgs,
                unsatisfied_dependents* simulate,
                const function<find_database_function>& fdb)
  {
    tracer trace ("execute_plan");

    l4 ([&]{trace << "simulate: " << (simulate ? "yes" : "no");});

    // If unsatisfied dependents are specified then we are in the simulation
    // mode and thus simulate can be used as bool.

    bool r (false);
    uint16_t verb (!simulate ? bpkg::verb : 0);

    bool result (verb && !o.no_result ());
    bool progress (!result &&
                   ((verb == 1 && !o.no_progress () && stderr_term) ||
                    o.progress ()));

    size_t prog_i, prog_n, prog_percent;

    // sys-install
    //
    // Install the system/distribution packages required by the respective
    // system packages (see build_package::system_install() for details).
    //
    if (!simulate && o.sys_install ())
    {
      // Collect the names of all the system packages being managed by the
      // system package manager (as opposed to user/fallback), suppressing
      // duplicates.
      //
      vector<package_name> ps;

      for (build_package& p: build_pkgs)
      {
        if (p.system_status () &&
            find (ps.begin (), ps.end (), p.name ()) == ps.end ())
        {
          ps.push_back (p.name ());
        }
      }

      // Install the system/distribution packages.
      //
      if (!ps.empty ())
      {
        // Otherwise, we wouldn't get any package statuses.
        //
        assert (sys_pkg_mgr && *sys_pkg_mgr != nullptr);

        (*sys_pkg_mgr)->install (ps);
      }
    }

    // disfigure
    //
    // Note: similar code in pkg-drop.
    //
    auto disfigure_pred = [] (const build_package& p)
    {
      // We are only interested in configured packages that are either being
      // up/down-graded, need reconfiguration (e.g., dependents), or dropped.
      //
      if (*p.action != build_package::drop && !p.reconfigure ())
        return false;

      return true;
    };

    if (progress)
    {
      prog_i = 0;
      prog_n = static_cast<size_t> (count_if (build_pkgs.begin (),
                                              build_pkgs.end (),
                                              disfigure_pred));
      prog_percent = 100;
    }

    // On the package reconfiguration we will try to resolve dependencies to
    // the same prerequisites (see pkg_configure() for details). For that, we
    // will save prerequisites before disfiguring a package. Note, though,
    // that this is not required for the recursively collected packages since
    // the dependency alternatives are already selected for them.
    //
    map<const build_package*, vector<package_name>> previous_prerequisites;

    for (build_package& p: build_pkgs)
    {
      assert (p.action);

      if (!disfigure_pred (p))
        continue;

      database& pdb (p.db);
      shared_ptr<selected_package>& sp (p.selected);

      assert (sp != nullptr); // Shouldn't be here otherwise.

      // Each package is disfigured in its own transaction, so that we
      // always leave the configuration in a valid state.
      //
      transaction t (pdb, !simulate /* start */);

      // Figure out if an external package is being replaced with another
      // external.
      //
      bool external (false);
      if (!simulate)
      {
        external = (sp->external () && p.external ());

        // Reset the keep_out flag if the package being unpacked is not
        // external.
        //
        if (p.keep_out && !external)
          p.keep_out = false;
      }

      // Save prerequisites before disfiguring the package.
      //
      // Note that we add the prerequisites list to the map regardless if
      // there are any prerequisites or not to, in particular, indicate the
      // package reconfiguration mode to the subsequent
      // pkg_configure_prerequisites() call (see the function documentation
      // for details).
      //
      if (*p.action != build_package::drop && !p.dependencies && !p.system)
      {
        vector<package_name>& ps (previous_prerequisites[&p]);

        if (!sp->prerequisites.empty ())
        {
          ps.reserve (sp->prerequisites.size ());

          for (const auto& pp: sp->prerequisites)
            ps.push_back (pp.first.object_id ());
        }
      }

      // For an external package being replaced with another external, keep
      // the configuration unless requested not to with --disfigure.
      //
      bool disfigure (p.disfigure || !external);

      // If the skeleton was not initialized yet (this is an existing package
      // reconfiguration and no configuration was printed as a part of the
      // plan, etc), then initialize it now. Whether the skeleton is newly
      // initialized or not, make sure that the current configuration is
      // loaded, unless the package project is not being disfigured.
      //
      if (*p.action != build_package::drop && !p.system)
      {
        if (!p.skeleton)
        {
          // If there is no available package specified for the build package
          // object, then we need to find it (or create a transient one).
          //
          p.init_skeleton (o,
                           true /* load_old_dependent_config */,
                           (p.available == nullptr
                            ? find_available (o, pdb, sp)
                            : nullptr));
        }

        if (disfigure)
          p.skeleton->load_old_config ();
      }

      // Commits the transaction.
      //
      pkg_disfigure (o, pdb, t,
                     sp,
                     !p.keep_out /* clean */,
                     disfigure,
                     simulate);

      r = true;

      assert (sp->state == package_state::unpacked ||
              sp->state == package_state::transient);

      if (result || progress)
      {
        const char* what (sp->state == package_state::transient
                          ? "purged"
                          : "disfigured");
        if (result)
          text << what << ' ' << *sp << pdb;
        else if (progress)
        {
          size_t p ((++prog_i * 100) / prog_n);

          if (prog_percent != p)
          {
            prog_percent = p;

            diag_progress_lock pl;
            diag_progress  = ' ';
            diag_progress += to_string (p);
            diag_progress += "% of packages ";
            diag_progress += what;
          }
        }
      }

      // Selected system package is now gone from the database. Before we drop
      // the object we need to make sure the hold state is preserved in the
      // package being reconfigured.
      //
      if (sp->state == package_state::transient)
      {
        if (!p.hold_package)
          p.hold_package = sp->hold_package;

        if (!p.hold_version)
          p.hold_version = sp->hold_version;

        sp = nullptr;
      }
    }

    // Clear the progress if shown.
    //
    if (progress)
    {
      diag_progress_lock pl;
      diag_progress.clear ();
    }

    // purge, fetch/unpack|checkout
    //
    pkg_checkout_cache checkout_cache (o);
    for (build_package& p: reverse_iterate (build_pkgs))
    {
      assert (p.action);

      database& pdb (p.db);

      shared_ptr<selected_package>& sp (p.selected);
      const shared_ptr<available_package>& ap (p.available);
      const lazy_shared_ptr<repository_fragment>& af (p.repository_fragment);

      // Purge the dropped or system package, fetch/unpack or checkout the
      // other one.
      //
      for (;;) // Breakout loop.
      {
        if (*p.action == build_package::drop)
        {
          // Note that the selected system package is gone once disfigured
          // (see above).
          //
          if (sp != nullptr)
          {
            assert (!sp->system ());

            transaction t (pdb, !simulate /* start */);
            pkg_purge (pdb, t, sp, simulate); // Commits the transaction.

            r = true;

            if (result)
              text << "purged " << *sp << pdb;

            sp = nullptr;
          }

          break;
        }

        if (*p.action == build_package::adjust) // Skip adjustments.
        {
          assert (ap == nullptr);
          break;
        }

        assert (ap != nullptr);

        // System package should not be fetched, it should only be configured
        // on the next stage. Here we need to purge selected non-system package
        // if present. Before we drop the object we need to make sure the hold
        // state is preserved for the package being reconfigured.
        //
        if (p.system)
        {
          if (sp != nullptr && !sp->system ())
          {
            transaction t (pdb, !simulate /* start */);
            pkg_purge (pdb, t, sp, simulate); // Commits the transaction.

            r = true;

            if (result)
              text << "purged " << *sp << pdb;

            if (!p.hold_package)
              p.hold_package = sp->hold_package;

            if (!p.hold_version)
              p.hold_version = sp->hold_version;

            sp = nullptr;
          }

          break;
        }

        // Fetch or checkout if this is a new package or if we are
        // up/down-grading or replacing.
        //
        if (sp == nullptr                         ||
            sp->version != p.available_version () ||
            p.replace ())
        {
          sp = nullptr; // For the directory case below.

          // Distinguish between the package and archive/directory cases.
          //
          const package_location& pl (ap->locations[0]); // Got to have one.

          if (pl.repository_fragment.object_id () != "") // Special root?
          {
            transaction t (pdb, !simulate /* start */);

            // Go through package repository fragments to decide if we should
            // fetch, checkout or unpack depending on the available repository
            // basis. Preferring a local one over the remotes and the dir
            // repository type over the others seems like a sensible thing to
            // do.
            //
            optional<repository_basis> basis;

            for (const package_location& l: ap->locations)
            {
              if (!rep_masked_fragment (l.repository_fragment))
              {
                const repository_location& rl (
                  l.repository_fragment.load ()->location);

                if (!basis || rl.local ()) // First or local?
                {
                  basis = rl.basis ();

                  if (rl.directory_based ())
                    break;
                }
              }
            }

            assert (basis); // Shouldn't be here otherwise.

            // All calls commit the transaction.
            //
            switch (*basis)
            {
            case repository_basis::archive:
              {
                sp = pkg_fetch (o,
                                pdb,
                                af.database (),
                                t,
                                ap->id.name,
                                p.available_version (),
                                true /* replace */,
                                simulate);
                break;
              }
            case repository_basis::version_control:
              {
                sp = p.checkout_root
                  ? pkg_checkout (checkout_cache,
                                  o,
                                  pdb,
                                  af.database (),
                                  t,
                                  ap->id.name,
                                  p.available_version (),
                                  *p.checkout_root,
                                  true /* replace */,
                                  p.checkout_purge,
                                  simulate)
                  : pkg_checkout (checkout_cache,
                                  o,
                                  pdb,
                                  af.database (),
                                  t,
                                  ap->id.name,
                                  p.available_version (),
                                  true /* replace */,
                                  simulate);
                break;
              }
            case repository_basis::directory:
              {
                sp = pkg_unpack (o,
                                 pdb,
                                 af.database (),
                                 t,
                                 ap->id.name,
                                 p.available_version (),
                                 true /* replace */,
                                 simulate);
                break;
              }
            }
          }
          // Directory case is handled by unpack.
          //
          else if (exists (pl.location))
          {
            transaction t (pdb, !simulate /* start */);

            sp = pkg_fetch (
              o,
              pdb,
              t,
              pl.location, // Archive path.
              true,        // Replace
              false,       // Don't purge; commits the transaction.
              simulate);
          }

          if (sp != nullptr) // Actually fetched or checked out something?
          {
            r = true;

            assert (sp->state == package_state::fetched ||
                    sp->state == package_state::unpacked);

            if (result)
            {
              const repository_location& rl (sp->repository_fragment);

              repository_basis basis (
                !rl.empty ()
                ? rl.basis ()
                : repository_basis::archive); // Archive path case.

              diag_record dr (text);

              switch (basis)
              {
              case repository_basis::archive:
                {
                  assert (sp->state == package_state::fetched);
                  dr << "fetched " << *sp << pdb;
                  break;
                }
              case repository_basis::directory:
                {
                  assert (sp->state == package_state::unpacked);
                  dr << "using " << *sp << pdb << " (external)";
                  break;
                }
              case repository_basis::version_control:
                {
                  assert (sp->state == package_state::unpacked);
                  dr << "checked out " << *sp << pdb;
                  break;
                }
              }
            }
          }
        }

        // Unpack if required. Note that the package can still be NULL if this
        // is the directory case (see the fetch code above).
        //
        if (sp == nullptr || sp->state == package_state::fetched)
        {
          if (sp != nullptr)
          {
            transaction t (pdb, !simulate /* start */);

            // Commits the transaction.
            //
            sp = pkg_unpack (o, pdb, t, ap->id.name, simulate);

            if (result)
              text << "unpacked " << *sp << pdb;
          }
          else
          {
            const package_location& pl (ap->locations[0]);
            assert (pl.repository_fragment.object_id () == ""); // Special root.

            transaction t (pdb, !simulate /* start */);
            sp = pkg_unpack (o,
                             pdb,
                             t,
                             path_cast<dir_path> (pl.location),
                             true,   // Replace.
                             false,  // Don't purge; commits the transaction.
                             simulate);

            if (result)
              text << "using " << *sp << pdb << " (external)";
          }

          r = true;

          assert (sp->state == package_state::unpacked);
        }

        break; // Get out from the breakout loop.
      }
    }
    checkout_cache.clear (); // Detect errors.

    // configure
    //
    auto configure_pred = [] (const build_package& p)
    {
      // Skip package drops.
      //
      if (*p.action == build_package::drop)
        return false;

      // We configure everything that isn't already configured.
      //
      if (p.selected != nullptr &&
          p.selected->state == package_state::configured)
        return false;

      return true;
    };

    // On the first pass collect all the build_package's to be configured and
    // calculate their configure_prerequisites_result's.
    //
    struct configure_package
    {
      reference_wrapper<build_package> pkg;

      // These are unused for system packages.
      //
      configure_prerequisites_result   res;
      build2::variable_overrides       ovrs;
    };
    vector<configure_package> configure_packages;
    configure_packages.reserve (build_pkgs.size ());

    // While at it also collect global configuration variable overrides from
    // each configure_prerequisites_result::config_variables and merge them
    // into configure_global_vars.
    //
    // @@ TODO: Note that the current global override semantics is quite
    //    broken in that we don't force reconfiguration of all the packages.
    //
#ifndef BPKG_OUTPROC_CONFIGURE
    strings configure_global_vars;
#endif

    // Return the "would be" state of packages that would be configured
    // by this stage.
    //
    function<find_package_state_function> configured_state (
      [&configure_packages] (const shared_ptr<selected_package>& sp)
      -> optional<pair<package_state, package_substate>>
      {
        for (const configure_package& cp: configure_packages)
        {
          const build_package& p (cp.pkg);

          if (p.selected == sp)
            return make_pair (
              package_state::configured,
              p.system ? package_substate::system : package_substate::none);
        }

        return nullopt;
      });

    for (build_package& p: reverse_iterate (build_pkgs))
    {
      assert (p.action);

      if (!configure_pred (p))
        continue;

      shared_ptr<selected_package>& sp (p.selected);
      const shared_ptr<available_package>& ap (p.available);

      // Collect the package.
      //
      // At this stage the package is either selected, in which case it's a
      // source code one, or just available, in which case it is a system
      // one. Note that a system package gets selected as being configured.
      //
      // NOTE: remember to update the preparation of the plan to be presented
      // to the user if changing anything here.
      //
      assert (sp != nullptr || p.system);

      database& pdb (p.db);
      transaction t (pdb, !simulate /* start */);

      // Show how we got here if things go wrong, for example selecting a
      // prerequisite is ambiguous due to the dependency package being
      // configured in multiple linked configurations.
      //
      auto g (
        make_exception_guard (
          [&p] ()
          {
            info << "while configuring " << p.name () << p.db;
          }));

      configure_prerequisites_result cpr;
      if (p.system)
      {
        // We have no choice but to configure system packages on the first
        // pass since otherwise there will be no selected package for
        // pkg_configure_prerequisites() to find. Luckily they have no
        // dependencies and so can be configured in any order. We will print
        // their progress/result on the second pass in the proper order.
        //
        // Note: commits the transaction.
        //
        sp = pkg_configure_system (ap->id.name,
                                   p.available_version (),
                                   pdb,
                                   t);
      }
      else
      {
        // Should only be called for packages whose prerequisites are saved.
        //
        auto prereqs = [&p, &previous_prerequisites] ()
        {
          auto i (previous_prerequisites.find (&p));
          assert (i != previous_prerequisites.end ());
          return &i->second;
        };

        // In the simulation mode unconstrain all the unsatisfactory
        // dependencies, if any, while configuring the dependent (see
        // build_packages::collect_dependents() for details).
        //
        // Note: must be called at most once.
        //
        auto unconstrain_deps = [simulate,
                                 &p,
                                 &trace,
                                 deps = vector<package_key> ()] () mutable
        {
          if (simulate)
          {
            unsatisfied_dependent* ud (
              simulate->find_dependent (package_key (p.db, p.name ())));

            if (ud != nullptr)
            {
              assert (deps.empty ());

              deps.reserve (ud->ignored_constraints.size ());

              for (const auto& c: ud->ignored_constraints)
              {
                l5 ([&]{trace << "while configuring dependent " << p.name ()
                              << p.db << " in simulation mode unconstrain ("
                              << c.dependency << ' ' << c.constraint << ')';});

                deps.emplace_back (c.dependency);
              }
            }
          }

          return !deps.empty () ? &deps : nullptr;
        };

        if (ap != nullptr)
        {
          assert (*p.action == build_package::build);

          // If the package prerequisites builds are collected, then use the
          // resulting package skeleton and the pre-selected dependency
          // alternatives.
          //
          // Note that we may not collect the package prerequisites builds if
          // the package is already configured but we still need to
          // reconfigure it due, for example, to an upgrade of its dependency.
          // In this case we pass to pkg_configure() the newly created package
          // skeleton which contains the package configuration variables
          // specified on the command line but (naturally) no reflection
          // configuration variables. Note, however, that in this case
          // pkg_configure() call will evaluate the reflect clauses itself and
          // so the proper reflection variables will still end up in the
          // package configuration.
          //
          // @@ Note that if we ever allow the user to override the
          //    alternative selection, this will break (and also if the user
          //    re-configures the package manually). Maybe that a good reason
          //    not to allow this? Or we could store this information in the
          //    database.
          //
          if (p.dependencies)
          {
            assert (p.skeleton);

            cpr = pkg_configure_prerequisites (o,
                                               pdb,
                                               t,
                                               *p.dependencies,
                                               &*p.alternatives,
                                               move (*p.skeleton),
                                               nullptr /* prev_prerequisites */,
                                               simulate,
                                               fdb,
                                               configured_state,
                                               unconstrain_deps ());
          }
          else
          {
            assert (p.skeleton); // Must be initialized before disfiguring.

            cpr = pkg_configure_prerequisites (o,
                                               pdb,
                                               t,
                                               ap->dependencies,
                                               nullptr /* alternatives */,
                                               move (*p.skeleton),
                                               prereqs (),
                                               simulate,
                                               fdb,
                                               configured_state,
                                               unconstrain_deps ());
          }
        }
        else // Existing dependent.
        {
          // This is an adjustment of a dependent which cannot be system
          // (otherwise it wouldn't be a dependent) and cannot become system
          // (otherwise it would be a build).
          //
          assert (*p.action == build_package::adjust && !sp->system ());

          // Must be in the unpacked state since it was disfigured on the
          // first pass (see above).
          //
          assert (sp->state == package_state::unpacked);

          // The skeleton must be initialized before disfiguring and the
          // package can't be system.
          //
          assert (p.skeleton && p.skeleton->available != nullptr);

          const dependencies& deps (p.skeleton->available->dependencies);

          // @@ Note that on reconfiguration the dependent looses the
          //    potential configuration variables specified by the user on
          //    some previous build, which can be quite surprising. Should we
          //    store this information in the database?
          //
          //    Note: this now works for external packages via package
          //    skeleton (which extracts user configuration).
          //
          cpr = pkg_configure_prerequisites (o,
                                             pdb,
                                             t,
                                             deps,
                                             nullptr /* alternatives */,
                                             move (*p.skeleton),
                                             prereqs (),
                                             simulate,
                                             fdb,
                                             configured_state,
                                             unconstrain_deps ());
        }

        t.commit ();

        if (verb >= 5 && !simulate && !cpr.config_variables.empty ())
        {
          diag_record dr (trace);

          dr << sp->name << pdb << " configuration variables:";

          for (const string& cv: cpr.config_variables)
            dr << "\n  " << cv;
        }

        if (!simulate)
        {
#ifndef BPKG_OUTPROC_CONFIGURE
          auto& gvs (configure_global_vars);

          // Note that we keep global overrides in cpr.config_variables for
          // diagnostics and skip them in var_override_function below.
          //
          for (const string& v: cpr.config_variables)
          {
            // Each package should have exactly the same set of global
            // overrides by construction since we don't allow package-
            // specific global overrides.
            //
            if (v[0] == '!')
            {
              if (find (gvs.begin (), gvs.end (), v) == gvs.end ())
                gvs.push_back (v);
            }
          }
#endif
          // Add config.config.disfigure unless already disfigured (see the
          // high-level pkg_configure() version for background).
          //
          if (ap == nullptr || !p.disfigure)
          {
            cpr.config_variables.push_back (
              "config.config.disfigure='config." + sp->name.variable () + "**'");
          }
        }
      }

      configure_packages.push_back (configure_package {p, move (cpr), {}});
    }

    // Reuse the build state to avoid reloading the dependencies over and over
    // again. This is a valid optimization since we are configuring in the
    // dependency-dependent order.
    //
    unique_ptr<build2::context> configure_ctx;

#ifndef BPKG_OUTPROC_CONFIGURE
    if (!simulate)
    {
      using build2::context;
      using build2::variable_override;

      function<context::var_override_function> vof (
        [&configure_packages] (context& ctx, size_t& i)
        {
          for (configure_package& cp: configure_packages)
          {
            for (const string& v: cp.res.config_variables)
            {
              if (v[0] == '!') // Skip global overrides (see above).
                continue;

              pair<char, variable_override> p (
                ctx.parse_variable_override (v, i++, false /* buildspec */));

              variable_override& vo (p.second);

              // @@ TODO: put absolute scope overrides into global_vars.
              //
              assert (!(p.first == '!' || (vo.dir && vo.dir->absolute ())));

              cp.ovrs.push_back (move (vo));
            }
          }
        });

      configure_ctx = pkg_configure_context (
        o, move (configure_global_vars), vof);

      // Only global in configure_global_vars.
      //
      assert (configure_ctx->var_overrides.empty ());
    }
#endif

    if (progress)
    {
      prog_i = 0;
      prog_n = configure_packages.size ();
      prog_percent = 100;
    }

    for (configure_package& cp: configure_packages)
    {
      build_package& p (cp.pkg);

      const shared_ptr<selected_package>& sp (p.selected);

      // Configure the package (system already configured).
      //
      // NOTE: remember to update the preparation of the plan to be presented
      // to the user if changing anything here.
      //
      database& pdb (p.db);

      if (!p.system)
      {
        const shared_ptr<available_package>& ap (p.available);

        transaction t (pdb, !simulate /* start */);

        // Show how we got here if things go wrong.
        //
        auto g (
          make_exception_guard (
            [&p] ()
            {
              info << "while configuring " << p.name () << p.db;
            }));

        // Note that pkg_configure() commits the transaction.
        //
        if (ap != nullptr)
        {
          pkg_configure (o,
                         pdb,
                         t,
                         sp,
                         move (cp.res),
                         configure_ctx,
                         cp.ovrs,
                         simulate);
        }
        else // Dependent.
        {
          pkg_configure (o,
                         pdb,
                         t,
                         sp,
                         move (cp.res),
                         configure_ctx,
                         cp.ovrs,
                         simulate);
        }
      }

      r = true;

      assert (sp->state == package_state::configured);

      if (result)
        text << "configured " << *sp << pdb;
      else if (progress)
      {
        size_t p ((++prog_i * 100) / prog_n);

        if (prog_percent != p)
        {
          prog_percent = p;

          diag_progress_lock pl;
          diag_progress  = ' ';
          diag_progress += to_string (p);
          diag_progress += "% of packages configured";
        }
      }
    }

#ifndef BPKG_OUTPROC_CONFIGURE
    configure_ctx.reset (); // Free.
#endif

    // Clear the progress if shown.
    //
    if (progress)
    {
      diag_progress_lock pl;
      diag_progress.clear ();
    }

    // Update the hold state.
    //
    // While we could have tried to "weave" it into one of the previous
    // actions, things there are already convoluted enough.
    //
    for (const build_package& p: reverse_iterate (build_pkgs))
    {
      assert (p.action);

      if (*p.action == build_package::drop)
        continue;

      database& pdb (p.db);

      const shared_ptr<selected_package>& sp (p.selected);
      assert (sp != nullptr);

      // Note that if not explicitly requested to unhold, we should only
      // "increase" the hold_package state. For version, if the user requested
      // upgrade to the (unspecified) latest, then we want to reset it.
      //
      bool hp (p.unhold ()
               ? false
               : p.hold_package
                 ? *p.hold_package
                 : sp->hold_package);

      bool hv (p.hold_version ? *p.hold_version : sp->hold_version);

      if (hp != sp->hold_package || hv != sp->hold_version)
      {
        sp->hold_package = hp;
        sp->hold_version = hv;

        transaction t (pdb, !simulate /* start */);
        pdb.update (sp);
        t.commit ();

        r = true;

        if (verb > 1)
        {
          if (hp)
            text << "holding package " << sp->name << pdb;

          if (hv)
            text << "holding version " << *sp << pdb;
        }
      }
    }

    return r;
  }
}
