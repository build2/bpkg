// file      : bpkg/pkg-build.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-build.hxx>

#include <map>
#include <set>
#include <list>
#include <cstring>    // strlen()
#include <iostream>   // cout

#include <libbutl/sha256.mxx>
#include <libbutl/standard-version.mxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/satisfaction.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/common-options.hxx>

#include <bpkg/cfg-link.hxx>
#include <bpkg/pkg-purge.hxx>
#include <bpkg/pkg-fetch.hxx>
#include <bpkg/rep-fetch.hxx>
#include <bpkg/cfg-create.hxx>
#include <bpkg/pkg-unpack.hxx>
#include <bpkg/pkg-update.hxx>
#include <bpkg/pkg-verify.hxx>
#include <bpkg/pkg-checkout.hxx>
#include <bpkg/pkg-configure.hxx>
#include <bpkg/pkg-disfigure.hxx>
#include <bpkg/system-repository.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  // @@ Overall TODO:
  //
  //    - Configuration vars (both passed and preserved)
  //

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

  // Configurations to use as the repository information sources.
  //
  // The list contains the current configurations and configurations of the
  // specified on the command line build-to-hold packages (ultimate
  // dependents).
  //
  // For ultimate dependents we use configurations in which they are being
  // built as a source of the repository information. For dependency packages
  // we use configurations of their ultimate dependents.
  //
  static linked_databases repo_configs;

  // Return the ultimate dependent configurations for packages in this
  // configuration.
  //
  static linked_databases
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

  // Retrieve the repository fragments for the specified package from its
  // ultimate dependent configurations and add them to the respective
  // configuration-associated fragment lists.
  //
  using config_repo_fragments =
    database_map<vector<shared_ptr<repository_fragment>>>;

  static void
  add_dependent_repo_fragments (database& db,
                                const available_package_id& id,
                                config_repo_fragments& r)
  {
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
          shared_ptr<repository_fragment> rf (pl.repository_fragment.load ());

          if (find (rfs.begin (), rfs.end (), rf) == rfs.end ())
            rfs.push_back (move (rf));
        }
      }
    }
  }

  // Try to find an available stub package in the imaginary system repository.
  // Such a repository contains stubs corresponding to the system packages
  // specified by the user on the command line with version information
  // (sys:libfoo/1.0, ?sys:libfoo/* but not ?sys:libfoo; the idea is that a
  // real stub won't add any extra information to such a specification so we
  // shouldn't insist on its presence). Semantically this imaginary repository
  // complements all real repositories.
  //
  static vector<shared_ptr<available_package>> imaginary_stubs;

  static shared_ptr<available_package>
  find_imaginary_stub (const package_name& name)
  {
    auto i (find_if (imaginary_stubs.begin (), imaginary_stubs.end (),
                     [&name] (const shared_ptr<available_package>& p)
                     {
                       return p->id.name == name;
                     }));

    return i != imaginary_stubs.end () ? *i : nullptr;
  }

  // Sort the available package fragments in the package version descending
  // order and suppress duplicate packages.
  //
  static void
  sort_dedup (vector<pair<shared_ptr<available_package>,
                          lazy_shared_ptr<repository_fragment>>>& pfs)
  {
    sort (pfs.begin (), pfs.end (),
          [] (const auto& x, const auto& y)
          {
            return x.first->version > y.first->version;
          });

    pfs.erase (unique (pfs.begin(), pfs.end(),
                      [] (const auto& x, const auto& y)
                      {
                        return x.first->version == y.first->version;
                      }),
               pfs.end ());
  }

  // Try to find packages that optionally satisfy the specified version
  // constraint in multiple databases, suppressing duplicates. Return the list
  // of packages and repository fragments in which each was found in the
  // package version descending or empty list if none were found. Note that a
  // stub satisfies any constraint.
  //
  // Note that we return (loaded) lazy_shared_ptr in order to also convey
  // the database to which it belongs.
  //
  static
  vector<pair<shared_ptr<available_package>,
              lazy_shared_ptr<repository_fragment>>>
  find_available (const linked_databases& dbs,
                  const package_name& name,
                  const optional<version_constraint>& c)
  {
    vector<pair<shared_ptr<available_package>,
                lazy_shared_ptr<repository_fragment>>> r;

    for (database& db: dbs)
    {
      for (shared_ptr<available_package> ap:
             pointer_result (query_available (db, name, c)))
      {
        // An available package should come from at least one fetched
        // repository fragment.
        //
        assert (!ap->locations.empty ());

        // All repository fragments the package comes from are equally good, so
        // we pick the first one.
        //
        r.emplace_back (move (ap), ap->locations[0].repository_fragment);
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

  // As above but only look for packages from the specified list of repository
  // fragments, their prerequisite repositories, and their complements,
  // recursively (note: recursivity applies to complements, not prerequisites).
  //
  static
  vector<pair<shared_ptr<available_package>,
              lazy_shared_ptr<repository_fragment>>>
  find_available (const package_name& name,
                  const optional<version_constraint>& c,
                  const config_repo_fragments& rfs,
                  bool prereq = true)
  {
    vector<pair<shared_ptr<available_package>,
                lazy_shared_ptr<repository_fragment>>> r;

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

  // As above but only look for a single package from the specified repository
  // fragment, its prerequisite repositories, and their complements,
  // recursively (note: recursivity applies to complements, not
  // prerequisites). Return the package and the repository fragment in which
  // it was found or NULL for both if not found.
  //
  // It is assumed that the repository fragment lazy pointer contains the
  // database information.
  //
  static pair<shared_ptr<available_package>,
              lazy_shared_ptr<repository_fragment>>
  find_available_one (const package_name& name,
                      const optional<version_constraint>& c,
                      const lazy_shared_ptr<repository_fragment>& rf,
                      bool prereq = true)
  {
    // Filter the result based on the repository fragment to which each
    // version belongs.
    //
    database& db (rf.database ());
    auto r (filter_one (rf.load (), query_available (db, name, c), prereq));

    if (r.first == nullptr)
      r.first = find_imaginary_stub (name);

    return make_pair (r.first,
                      (r.second != nullptr
                       ? lazy_shared_ptr<repository_fragment> (db,
                                                               move (r.second))
                       : nullptr));
  }

  // As above but look for a single package from a list of repository
  // fragments.
  //
  static pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>
  find_available_one (database& db,
                      const package_name& name,
                      const optional<version_constraint>& c,
                      const vector<shared_ptr<repository_fragment>>& rfs,
                      bool prereq = true)
  {
    // Filter the result based on the repository fragments to which each
    // version belongs.
    //
    auto r (filter_one (rfs, query_available (db, name, c), prereq));

    if (r.first == nullptr)
      r.first = find_imaginary_stub (name);

    return r;
  }

  // As above but look for a single package in multiple databases from their
  // respective root repository fragments.
  //
  static pair<shared_ptr<available_package>,
              lazy_shared_ptr<repository_fragment>>
  find_available_one (const linked_databases& dbs,
                      const package_name& name,
                      const optional<version_constraint>& c,
                      bool prereq = true)
  {
    for (database& db: dbs)
    {
      auto r (filter_one (db.load<repository_fragment> (""),
                          query_available (db, name, c),
                          prereq));

      if (r.first != nullptr)
        return make_pair (
          move (r.first),
          lazy_shared_ptr<repository_fragment> (db, move (r.second)));
    }

    return make_pair (find_imaginary_stub (name), nullptr);
  }

  // Create a transient (or fake, if you prefer) available_package object
  // corresponding to the specified selected object. Note that the package
  // locations list is left empty and that the returned repository fragment
  // could be NULL if the package is an orphan.
  //
  // Note also that in our model we assume that make_available() is only
  // called if there is no real available_package. This makes sure that if
  // the package moves (e.g., from testing to stable), then we will be using
  // stable to resolve its dependencies.
  //
  static pair<shared_ptr<available_package>,
              lazy_shared_ptr<repository_fragment>>
  make_available (const common_options& options,
                  database& db,
                  const shared_ptr<selected_package>& sp)
  {
    assert (sp != nullptr && sp->state != package_state::broken);

    if (sp->system ())
      return make_pair (make_shared<available_package> (sp->name, sp->version),
                        nullptr);

    // First see if we can find its repository fragment.
    //
    // Note that this is package's "old" repository fragment and there is no
    // guarantee that its dependencies are still resolvable from it. But this
    // is our best chance (we could go nuclear and point all orphans to the
    // root repository fragment but that feels a bit too drastic at the
    // moment).
    //
    shared_ptr<repository_fragment> af (
      db.find<repository_fragment> (
        sp->repository_fragment.canonical_name ()));

    // The package is in at least fetched state, which means we should
    // be able to get its manifest.
    //
    const optional<path>& a (sp->archive);

    package_manifest m (
      sp->state == package_state::fetched
      ? pkg_verify (options,
                    a->absolute () ? *a : db.config_orig / *a,
                    true /* ignore_unknown */,
                    false /* expand_values */)
      : pkg_verify (sp->effective_src_root (db.config_orig),
                    true /* ignore_unknown */,
                    // Copy potentially fixed up version from selected package.
                    [&sp] (version& v) {v = sp->version;}));

    return make_pair (make_shared<available_package> (move (m)),
                      (af != nullptr
                       ? lazy_shared_ptr<repository_fragment> (db, move (af))
                       : nullptr));
  }

  // Return true if the version constraint represents the wildcard version.
  //
  static inline bool
  wildcard (const version_constraint& vc)
  {
    bool r (vc.min_version && *vc.min_version == wildcard_version);

    if (r)
      assert (vc.max_version == vc.min_version);

    return r;
  }

  // The current configurations dependents being "repointed" to prerequisites
  // in other configurations, together with their replacement flags. The flag
  // is true for the replacement prerequisites ("new") and false for the
  // prerequisites being replaced ("old"). The unamended prerequisites have no
  // entries.
  //
  using repointed_dependents = map<config_package, map<config_package, bool>>;

  // List of the private configuration paths, relative to the containing
  // configuration directories (.bpkg/host/, etc), together with the
  // containing configuration databases.
  //
  using private_configs = vector<pair<database&, dir_path>>;

  // A "dependency-ordered" list of packages and their prerequisites.
  // That is, every package on the list only possibly depending on the
  // ones after it. In a nutshell, the usage is as follows: we first
  // add one or more packages (the "initial selection"; for example, a
  // list of packages the user wants built). The list then satisfies all
  // the prerequisites of the packages that were added, recursively. At
  // the end of this process we have an ordered list of all the packages
  // that we have to build, from last to first, in order to build our
  // initial selection.
  //
  // This process is split into two phases: satisfaction of all the
  // dependencies (the collect_build() function) and ordering of the list
  // (the order() function).
  //
  // During the satisfaction phase, we collect all the packages, their
  // prerequisites (and so on, recursively) in a map trying to satisfy
  // any version constraints. Specifically, during this step, we may
  // "upgrade" or "downgrade" a package that is already in a map as a
  // result of another package depending on it and, for example, requiring
  // a different version. One notable side-effect of this process is that
  // we may end up with a lot more packages in the map (but not in the list)
  // than we will have on the list. This is because some of the prerequisites
  // of "upgraded" or "downgraded" packages may no longer need to be built.
  //
  // Note also that we don't try to do exhaustive constraint satisfaction
  // (i.e., there is no backtracking). Specifically, if we have two
  // candidate packages each satisfying a constraint of its dependent
  // package, then if neither of them satisfy both constraints, then we
  // give up and ask the user to resolve this manually by explicitly
  // specifying the version that will satisfy both constraints.
  //
  struct build_package
  {
    enum action_type
    {
      // Available package is not NULL.
      //
      build,

      // Selected package is not NULL, available package is NULL.
      //
      drop,

      // Selected package is not NULL, available package is NULL.
      //
      // This is the "only adjustments" action for a selected package.
      // Adjustment flags (see below) are unhold (the package should be
      // treated as a dependency) and reconfigure (dependent package that
      // needs to be reconfigured because its prerequisite is being
      // up/down-graded or reconfigured).
      //
      // Note that this action is "replaceable" with either drop or build
      // action but in the latter case the adjustments must be copied over.
      //
      adjust
    };

    // An object with an absent action is there to "pre-enter" information
    // about a package (constraints and flags) in case it is used.
    //
    optional<action_type> action;

    reference_wrapper<database> db;           // Needs to be move-assignable.

    shared_ptr<selected_package>  selected;   // NULL if not selected.
    shared_ptr<available_package> available;  // Can be NULL, fake/transient.

    // Can be NULL (orphan) or root. If not NULL, then loaded from the
    // repository configuration database, which may differ from the
    // configuration the package is being built in.
    //
    lazy_shared_ptr<bpkg::repository_fragment> repository_fragment;

    const package_name&
    name () const
    {
      return selected != nullptr ? selected->name : available->id.name;
    }

    // Hold flags. Note that we only "increase" the hold_package value that is
    // already in the selected package.
    //
    optional<bool> hold_package;
    optional<bool> hold_version;

    // Constraint value plus, normally, the dependent package name that placed
    // this constraint but can also be some other name for the initial
    // selection (e.g., package version specified by the user on the command
    // line). This why we use the string type, rather than package_name.
    //
    struct constraint_type
    {
      reference_wrapper<database> db; // Main database for non-packages.
      string dependent;
      version_constraint value;

      constraint_type (database& d, string dp, version_constraint v)
          : db (d), dependent (move (dp)), value (move (v)) {}
    };

    vector<constraint_type> constraints;

    // System package indicator. See also a note in the merge() function.
    //
    bool system;

    // If this flag is set and the external package is being replaced with an
    // external one, then keep its output directory between upgrades and
    // downgrades.
    //
    bool keep_out;

    // If this flag is set then disfigure the package between upgrades and
    // downgrades effectively causing a from-scratch reconfiguration.
    //
    bool disfigure;

    // If this flag is set, then don't build this package, only configure.
    //
    // Note: use configure_only() to query.
    //
    bool configure_only_;

    // If present, then check out the package into the specified directory
    // rather than into the configuration directory, if it comes from a
    // version control-based repository. Optionally, remove this directory
    // when the package is purged.
    //
    optional<dir_path> checkout_root;
    bool               checkout_purge;

    // Command line configuration variables. Only meaningful for non-system
    // packages.
    //
    strings config_vars;

    // Set of packages (dependents or dependencies but not a mix) that caused
    // this package to be built or adjusted. Empty name signifies user
    // selection and can be present regardless of the required_by_dependents
    // flag value.
    //
    set<config_package> required_by;

    // If this flags is true, then required_by contains dependents.
    //
    // We need this because required_by packages have different semantics for
    // different actions: the dependent for regular builds and dependency for
    // adjustments and repointed dependent reconfiguration builds. Mixing them
    // would break prompts/diagnostics.
    //
    bool required_by_dependents;

    bool
    user_selection () const
    {
      return required_by.find (config_package {db.get ().main_database (),
                                               ""}) != required_by.end ();
    }

    // State flags.
    //
    uint16_t flags;

    // Set if we also need to clear the hold package flag.
    //
    static const uint16_t adjust_unhold = 0x0001;

    bool
    unhold () const
    {
      return (flags & adjust_unhold) != 0;
    }

    // Set if we also need to reconfigure this package. Note that in some
    // cases reconfigure is naturally implied. For example, if an already
    // configured package is being up/down-graded. For such cases we don't
    // guarantee that the reconfigure flag is set. We only make sure to set it
    // for cases that would otherwise miss the need for reconfiguration. As a
    // result, use the reconfigure() predicate which detects both explicit and
    // implied cases.
    //
    // At first, it may seem that this flag is redundant and having the
    // available package set to NULL is sufficient. But consider the case
    // where the user asked us to build a package that is already in the
    // configured state (so all we have to do is pkg-update). Next, add to
    // this a prerequisite package that is being upgraded. Now our original
    // package has to be reconfigured. But without this flag we won't know
    // (available for our package won't be NULL).
    //
    static const uint16_t adjust_reconfigure = 0x0002;

    bool
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

    // Set if this build action is for repointing of prerequisite.
    //
    static const uint16_t build_repoint = 0x0004;

    bool
    configure_only () const
    {
      assert (action);

      return configure_only_ ||
        (*action == build && (flags & build_repoint) != 0);
    }

    const version&
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

    string
    available_name_version () const
    {
      assert (available != nullptr);
      return package_string (available->id.name,
                             available_version (),
                             system);
    }

    string
    available_name_version_db () const
    {
      const string& s (db.get ().string);
      return !s.empty ()
             ? available_name_version () + ' ' + s
             : available_name_version ();
    }

    // Merge constraints, required-by package names, hold_* flags, state
    // flags, and user-specified options/variables.
    //
    void
    merge (build_package&& p)
    {
      // We don't merge objects from different configurations.
      //
      assert (db == p.db);

      // We don't merge into pre-entered objects, and from/into drops.
      //
      assert (action && *action != drop && (!p.action || *p.action != drop));

      // Copy the user-specified options/variables.
      //
      if (p.user_selection ())
      {
        // We don't allow a package specified on the command line multiple
        // times to have different sets of options/variables. Given that, it's
        // tempting to assert that the options/variables don't change if we
        // merge into a user selection. That's, however, not the case due to
        // the iterative plan refinement implementation details (--checkout-*
        // options and variables are only saved into the pre-entered
        // dependencies, etc.).
        //
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

      if (*action == build)
      {
        // We never merge two repointed dependent reconfigurations.
        //
        assert ((flags & build_repoint) == 0 ||
                (p.flags & build_repoint) == 0);

        // Upgrade repoint to the full build.
        //
        flags &= ~build_repoint;
      }

      // Note that we don't copy the build_package::system flag. If it was
      // set from the command line ("strong system") then we will also have
      // the '==' constraint which means that this build_package object will
      // never be replaced.
      //
      // For other cases ("weak system") we don't want to copy system over in
      // order not prevent, for example, system to non-system upgrade.
    }
  };

  using build_package_list = list<reference_wrapper<build_package>>;

  using build_package_refs =
    small_vector<reference_wrapper<const build_package>, 16>;

  using add_priv_cfg_function = void (database&, dir_path&&);

  struct build_packages: build_package_list
  {
    // Packages collection of whose prerequisites has been postponed due the
    // inability to find a version satisfying the pre-entered constraint from
    // repositories available to this package. The idea is that this
    // constraint could still be satisfied from a repository fragment of some
    // other package (that we haven't processed yet) that also depends on this
    // prerequisite.
    //
    using postponed_packages = set<const build_package*>;

    // Pre-enter a build_package without an action. No entry for this package
    // may already exists.
    //
    void
    enter (package_name name, build_package pkg)
    {
      assert (!pkg.action);

      database& db (pkg.db); // Save before the move() call.
      auto p (map_.emplace (config_package {db, move (name)},
                            data_type {end (), move (pkg)}));

      assert (p.second);
    }

    // Return true if a package is already in the map.
    //
    bool
    entered (database& db, const package_name& name)
    {
      return map_.find (db, name) != map_.end ();
    }

    // Collect the package being built. Return its pointer if this package
    // version was, in fact, added to the map and NULL if it was already there
    // or the existing version was preferred. So can be used as bool.
    //
    // Also, in the recursive mode:
    //
    // - Use the custom search function to find the package dependency
    //   databases.
    //
    // - For the repointed dependents collect the prerequisite replacements
    //   rather than prerequisites being replaced.
    //
    // - Add paths of the created private configurations, together with the
    //   containing configuration databases, into the specified list (see
    //   private_configs for details).
    //
    build_package*
    collect_build (const pkg_build_options& options,
                   build_package pkg,
                   const function<find_database_function>& fdb,
                   const repointed_dependents& rpt_depts,
                   const function<add_priv_cfg_function>& apc,
                   postponed_packages* recursively = nullptr,
                   build_package_refs* dep_chain = nullptr)
    {
      using std::swap; // ...and not list::swap().

      tracer trace ("collect_build");

      // Must both be either specified or not.
      //
      assert ((recursively == nullptr) == (dep_chain == nullptr));

      // Only builds are allowed here.
      //
      assert (pkg.action && *pkg.action == build_package::build &&
              pkg.available != nullptr);

      auto i (map_.find (pkg.db, pkg.available->id.name));

      // If we already have an entry for this package name, then we
      // have to pick one over the other.
      //
      // If the existing entry is a pre-entered or is non-build one, then we
      // merge it into the new build entry. Otherwise (both are builds), we
      // pick one and merge the other into it.
      //
      if (i != map_.end ())
      {
        build_package& bp (i->second.package);

        // Can't think of the scenario when this happens. We would start
        // collecting from scratch (see below).
        //
        assert (!bp.action || *bp.action != build_package::drop);

        if (!bp.action || *bp.action != build_package::build) // Non-build.
        {
          pkg.merge (move (bp));
          bp = move (pkg);
        }
        else                                                  // Build.
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

          // See if we are replacing the object. If not, then we don't
          // need to collect its prerequisites since that should have
          // already been done. Remember, p1 points to the object we
          // want to keep.
          //
          bool replace (p1 != &i->second.package);

          if (replace)
          {
            swap (*p1, *p2);
            swap (p1, p2); // Setup for merge below.
          }

          p1->merge (move (*p2));

          if (!replace)
            return nullptr;
        }
      }
      else
      {
        // This is the first time we are adding this package name to the map.
        //
        l4 ([&]{trace << "add " << pkg.available_name_version_db ();});

        // Note: copy; see emplace() below.
        //
        database& db (pkg.db); // Save before the move() call.
        package_name n (pkg.available->id.name);
        i = map_.emplace (config_package {db, move (n)},
                          data_type {end (), move (pkg)}).first;
      }

      build_package& p (i->second.package);

      // Recursively collect build prerequisites, if requested.
      //
      // Note that detecting dependency cycles during the satisfaction phase
      // would be premature since they may not be present in the final package
      // list. Instead we check for them during the ordering phase.
      //
      // The question, of course, is whether we can still end up with an
      // infinite recursion here? Note that for an existing map entry we only
      // recurse after the entry replacement. The infinite recursion would
      // mean that we may replace a package in the map with the same version
      // multiple times:
      //
      // ... p1 -> p2 -> ... p1
      //
      // Every replacement increases the entry version and/or tightens the
      // constraints the next replacement will need to satisfy. It feels
      // impossible that a package version can "return" into the map being
      // replaced once. So let's wait until some real use case proves this
      // reasoning wrong.
      //
      if (recursively != nullptr)
        collect_build_prerequisites (options,
                                     p,
                                     recursively,
                                     fdb,
                                     rpt_depts,
                                     apc,
                                     *dep_chain);

      return &p;
    }

    // Collect prerequisites of the package being built recursively.
    //
    // But first "prune" this process if the package we build is a system one
    // or is already configured and is not a repointed dependent, since that
    // would mean all its prerequisites are configured as well. Note that this
    // is not merely an optimization: the package could be an orphan in which
    // case the below logic will fail (no repository fragment in which to
    // search for prerequisites). By skipping the prerequisite check we are
    // able to gracefully handle configured orphans.
    //
    // For the repointed dependent, we still need to collect its prerequisite
    // replacements to make sure its constraints over them are satisfied. Note
    // that, as it was said above, we can potentially fail if the dependent is
    // an orphan, but this is exactly what we need to do in that case, since
    // we won't be able to be reconfigure it anyway.
    //
    void
    collect_build_prerequisites (const pkg_build_options& options,
                                 const build_package& pkg,
                                 postponed_packages* postponed,
                                 const function<find_database_function>& fdb,
                                 const repointed_dependents& rpt_depts,
                                 const function<add_priv_cfg_function>& apc,
                                 build_package_refs& dep_chain)
    {
      tracer trace ("collect_build_prerequisites");

      assert (pkg.action && *pkg.action == build_package::build);

      if (pkg.system)
        return;

      database& pdb (pkg.db);
      const shared_ptr<selected_package>& sp (pkg.selected);

      // True if this is an up/down-grade.
      //
      bool ud (false);

      // If this is a repointed dependent, then it points to its prerequisite
      // replacements flag map (see repointed_dependents for details).
      //
      const map<config_package, bool>* rpt_prereq_flags (nullptr);

      // Bail out if this is a configured non-system package and no
      // up/down-grade nor collecting prerequisite replacements are required.
      //
      if (sp != nullptr                            &&
          sp->state == package_state::configured   &&
          sp->substate != package_substate::system)
      {
        ud = sp->version != pkg.available_version ();

        repointed_dependents::const_iterator i (
          rpt_depts.find (config_package {pdb, sp->name}));

        if (i != rpt_depts.end ())
          rpt_prereq_flags = &i->second;

        if (!ud && rpt_prereq_flags == nullptr)
         return;
      }

      dep_chain.push_back (pkg);

      // Show how we got here if things go wrong.
      //
      // To suppress printing this information clear the dependency chain
      // before throwing an exception.
      //
      auto g (
        make_exception_guard (
          [&dep_chain] ()
          {
            // Note that we also need to clear the dependency chain, to
            // prevent the caller's exception guard from printing it.
            //
            while (!dep_chain.empty ())
            {
              info << "while satisfying "
                   << dep_chain.back ().get ().available_name_version_db ();

              dep_chain.pop_back ();
            }
          }));

      const shared_ptr<available_package>& ap (pkg.available);
      const lazy_shared_ptr<repository_fragment>& af (pkg.repository_fragment);
      const package_name& name (ap->id.name);

      for (const dependency_alternatives_ex& da: ap->dependencies)
      {
        if (da.conditional) // @@ TODO
          fail << "conditional dependencies are not yet supported";

        if (da.size () != 1) // @@ TODO
          fail << "multiple dependency alternatives not yet supported";

        const dependency& dp (da.front ());
        const package_name& dn (dp.name);

        if (da.buildtime)
        {
          // Handle special names.
          //
          if (dn == "build2")
          {
            if (dp.constraint)
              satisfy_build2 (options, name, dp);

            continue;
          }
          else if (dn == "bpkg")
          {
            if (dp.constraint)
              satisfy_bpkg (options, name, dp);

            continue;
          }
          else if (pdb.type == build2_config_type)
          {
            // Note that the dependent is not necessarily a build system
            // module.
            //
            fail << "build-time dependency " << dn << " in build system "
                 << "module configuration" <<
              info << "build system modules cannot have build-time "
                   << "dependencies";
          }
        }

        bool system (false);
        bool dep_optional (false);

        // If the user specified the desired dependency version constraint,
        // then we will use it to overwrite the constraint imposed by the
        // dependent package, checking that it is still satisfied.
        //
        // Note that we can't just rely on the execution plan refinement that
        // will pick up the proper dependency version at the end of the day.
        // We may just not get to the plan execution simulation, failing due
        // to inability for dependency versions collected by two dependents to
        // satisfy each other constraints (for an example see the
        // pkg-build/dependency/apply-constraints/resolve-conflict{1,2}
        // tests).

        // Points to the desired dependency version constraint, if specified,
        // and is NULL otherwise. Can be used as boolean flag.
        //
        const version_constraint* dep_constr (nullptr);

        database* ddb (fdb (pdb, dn, da.buildtime));

        auto i (ddb != nullptr
                ? map_.find (*ddb, dn)
                : map_.find_dependency (pdb, dn, da.buildtime));

        if (i != map_.end ())
        {
          const build_package& bp (i->second.package);

          dep_optional = !bp.action; // Is pre-entered.

          if (dep_optional &&
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
              fail << "unable to satisfy constraints on package " << dn <<
                info << name << pdb << " depends on (" << dn << " "
                   << *dp.constraint << ")" <<
                info << c.dependent << c.db << " depends on (" << dn << " "
                     << c.value << ")" <<
                info << "specify " << dn << " version to satisfy " << name
                     << " constraint";
          }
        }

        const dependency& d (!dep_constr
                             ? dp
                             : dependency {dn, *dep_constr});

        // First see if this package is already selected. If we already have
        // it in the configuration and it satisfies our dependency version
        // constraint, then we don't want to be forcing its upgrade (or,
        // worse, downgrade).
        //
        // If the prerequisite configuration is explicitly specified by the
        // user, then search for the prerequisite in this specific
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
          : find_dependency (pdb, dn, da.buildtime));

        if (ddb == nullptr)
          ddb = &pdb;

        shared_ptr<selected_package>& dsp (spd.first);

        pair<shared_ptr<available_package>,
             lazy_shared_ptr<repository_fragment>> rp;

        shared_ptr<available_package>& dap (rp.first);

        bool force (false);

        if (dsp != nullptr)
        {
          // Switch to the selected package configuration.
          //
          ddb = spd.second;

          // If we are collecting prerequisites of the repointed dependent,
          // then only proceed further if this is either a replacement or
          // unamended prerequisite and we are up/down-grading (only for the
          // latter).
          //
          if (rpt_prereq_flags != nullptr)
          {
            auto i (rpt_prereq_flags->find (config_package {*ddb, dn}));

            bool unamended   (i == rpt_prereq_flags->end ());
            bool replacement (!unamended && i->second);

            // We can never end up with the prerequisite being replaced, since
            // the fdb() function should always return the replacement instead
            // (see above).
            //
            assert (unamended || replacement);

            if (!(replacement || (unamended && ud)))
              continue;
          }

          if (dsp->state == package_state::broken)
            fail << "unable to build broken package " << dn << *ddb <<
              info << "use 'pkg-purge --force' to remove";

          // If the constraint is imposed by the user we also need to make sure
          // that the system flags are the same.
          //
          if (satisfies (dsp->version, d.constraint) &&
              (!dep_constr || dsp->system () == system))
          {
            system = dsp->system ();

            // First try to find an available package for this exact version.
            // In particular, this handles the case where a package moves from
            // one repository to another (e.g., from testing to stable). For a
            // system package we pick the latest one (its exact version
            // doesn't really matter).
            //
            rp = find_available_one (dependent_repo_configs (*ddb),
                                     dn,
                                     (!system
                                      ? version_constraint (dsp->version)
                                      : optional<version_constraint> ()));

            // A stub satisfies any version constraint so we weed them out
            // (returning stub as an available package feels wrong).
            //
            if (dap == nullptr || dap->stub ())
              rp = make_available (options, *ddb, dsp);
          }
          else
            // Remember that we may be forcing up/downgrade; we will deal with
            // it below.
            //
            force = true;
        }

        // If this is a build-time dependency and we build it for the first
        // time, then we need to find a suitable configuration (of the host or
        // build2 type) to build it in.
        //
        // If the current configuration (ddb) is of the suitable type, then we
        // use that. Otherwise, we go through its immediate explicit links. If
        // only one of them has the suitable type, then we use that. If there
        // are multiple of them, then we fail advising the user to pick one
        // explicitly. If there are none, then we create the private
        // configuration and use that. If the current configuration is
        // private, then search/create in the parent configuration instead.
        //
        // Note that if the user has explicitly specified the configuration
        // for this dependency on the command line (using --config-*), then
        // this configuration is used as the starting point for this search.
        //
        if (da.buildtime   &&
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
                fail << "multiple possible " << type << " configurations for "
                     << "build-time dependency (" << dp << ")" <<
                  info << db->config_orig <<
                  info << ldb.config_orig <<
                  info << "use --config-* to select the configuration";
            }
          }

          // If no suitable configuration is found, then create and link it,
          // unless the --no-private-config options is specified. In the
          // latter case, print the dependency chain to stdout and exit with
          // the specified code.
          //
          if (db == nullptr)
          {
            if (options.no_private_config_specified ())
            try
            {
              // Note that we don't have the dependency package version yet.
              // We could probably rearrange the code and obtain the available
              // dependency package by now, given that it comes from the main
              // database and may not be specified as system (we would have
              // the configuration otherwise). However, let's not complicate
              // the code further and instead print the package name and the
              // constraint, if present.
              //
              // Also, in the future, we may still need the configuration to
              // obtain the available dependency package for some reason (may
              // want to fetch repositories locally, etc).
              //
              cout << d << '\n';

              // Note that we also need to clean the dependency chain, to
              // prevent the exception guard from printing it to stderr.
              //
              for (build_package_refs dc (move (dep_chain)); !dc.empty (); )
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
            // left from a previous faulty run. Note that trying to reuse it
            // would be a bad idea since it can be half-prepared, with an
            // outdated database schema version, etc.
            //
            cfg_create (options,
                        sdb.config_orig / cd,
                        optional<string> (type) /* name */,
                        type                    /* type */,
                        mods,
                        vars,
                        false                   /* existing */,
                        true                    /* wipe */);

            // Note that we will copy the name from the configuration unless
            // it clashes with one of the existing links.
            //
            shared_ptr<configuration> lc (cfg_link (sdb,
                                                    sdb.config / cd,
                                                    true    /* relative */,
                                                    nullopt /* name */,
                                                    true    /* sys_rep */));

            // Save the newly-created private configuration, together with the
            // containing configuration database, for their subsequent re-
            // link.
            //
            apc (sdb, move (cd));

            db = &sdb.find_attached (*lc->id);
          }

          ddb = db; // Switch to the dependency configuration.
        }

        // Note that building a dependent which is not a build2 module in the
        // same configuration with the build2 module it depends upon is an
        // error.
        //
        if (da.buildtime          &&
            !build2_module (name) &&
            build2_module (dn)    &&
            pdb == *ddb)
        {
          // Note that the dependent package information is printed by the
          // above exception guard.
          //
          fail << "unable to build build system module " << dn << " in its "
               << "dependent package configuration " << pdb.config_orig <<
            info << "use --config-* to select suitable configuration";
        }

        // If we didn't get the available package corresponding to the
        // selected package, look for any that satisfies the constraint.
        //
        if (dap == nullptr)
        {
          // And if we have no repository fragment to look in, then that means
          // the package is an orphan (we delay this check until we actually
          // need the repository fragment to allow orphans without
          // prerequisites).
          //
          if (af == nullptr)
            fail << "package " << pkg.available_name_version_db ()
                 << " is orphaned" <<
              info << "explicitly upgrade it to a new version";

          // We look for prerequisites only in the repositories of this
          // package (and not in all the repositories of this configuration).
          // At first this might look strange, but it also kind of makes
          // sense: we only use repositories "approved" for this package
          // version. Consider this scenario as an example: hello/1.0.0 and
          // libhello/1.0.0 in stable and libhello/2.0.0 in testing. As a
          // prerequisite of hello, which version should libhello resolve to?
          // While one can probably argue either way, resolving it to 1.0.0 is
          // the conservative choice and the user can always override it by
          // explicitly building libhello.
          //
          // Note though, that if this is a test package, then its special
          // test dependencies (main packages that refer to it) should be
          // searched upstream through the complement repositories
          // recursively, since the test packages may only belong to the main
          // package's repository and its complements.
          //
          // @@ Currently we don't implement the reverse direction search for
          //    the test dependencies, effectively only supporting the common
          //    case where the main and test packages belong to the same
          //    repository. Will need to fix this eventually.
          //
          // Note that this logic (naturally) does not apply if the package is
          // already selected by the user (see above).
          //
          // Also note that for the user-specified dependency version
          // constraint we rely on the satisfying package version be present
          // in repositories of the first dependent met. As a result, we may
          // fail too early if such package version doesn't belong to its
          // repositories, but belongs to the ones of some dependent that
          // we haven't met yet. Can we just search all repositories for an
          // available package of the appropriate version and just take it,
          // if present? We could, but then which repository should we pick?
          // The wrong choice can introduce some unwanted repositories and
          // package versions into play. So instead, we will postpone
          // collecting the problematic dependent, expecting that some other
          // one will find the appropriate version in its repositories.
          //
          // For a system package we pick the latest version just to make sure
          // the package is recognized. An unrecognized package means the
          // broken/stale repository (see below).
          //
          rp = find_available_one (dn, !system ? d.constraint : nullopt, af);

          if (dap == nullptr)
          {
            if (dep_constr && !system && postponed)
            {
              postponed->insert (&pkg);
              break;
            }

            diag_record dr (fail);
            dr << "unknown dependency " << dn;

            // We need to be careful not to print the wildcard-based
            // constraint.
            //
            if (d.constraint && (!dep_constr || !wildcard (*dep_constr)))
              dr << ' ' << *d.constraint;

            dr << " of package " << name << pdb;

            if (!af->location.empty () && (!dep_constr || system))
              dr << info << "repository " << af->location << " appears to "
                 << "be broken" <<
                info << "or the repository state could be stale" <<
                info << "run 'bpkg rep-fetch' to update";
          }

          // If all that's available is a stub then we need to make sure the
          // package is present in the system repository and it's version
          // satisfies the constraint. If a source package is available but
          // there is a system package specified on the command line and it's
          // version satisfies the constraint then the system package should
          // be preferred. To recognize such a case we just need to check if
          // the authoritative system version is set and it satisfies the
          // constraint. If the corresponding system package is non-optional
          // it will be preferred anyway.
          //
          if (dap->stub ())
          {
            // Note that the constraint can safely be printed as it can't
            // be a wildcard (produced from the user-specified dependency
            // version constraint). If it were, then the system version
            // wouldn't be NULL and would satisfy itself.
            //
            if (dap->system_version (*ddb) == nullptr)
              fail << "dependency " << d << " of package " << name << " is "
                   << "not available in source" <<
                info << "specify ?sys:" << dn << " if it is available from "
                   << "the system";

            if (!satisfies (*dap->system_version (*ddb), d.constraint))
              fail << "dependency " << d << " of package " << name << " is "
                   << "not available in source" <<
                info << package_string (dn,
                                        *dap->system_version (*ddb),
                                        true /* system */)
                   << " does not satisfy the constrains";

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

        build_package bp {
          build_package::build,
          *ddb,
          dsp,
          dap,
          move (rp.second),
          nullopt,                      // Hold package.
          nullopt,                      // Hold version.
          {},                           // Constraints.
          system,
          false,                        // Keep output directory.
          false,                        // Disfigure (from-scratch reconf).
          false,                        // Configure-only.
          nullopt,                      // Checkout root.
          false,                        // Checkout purge.
          strings (),                   // Configuration variables.
          {config_package {pdb, name}}, // Required by (dependent).
          true,                         // Required by dependents.
          0};                           // State flags.

        // Add our constraint, if we have one.
        //
        // Note that we always add the constraint implied by the dependent.
        // The user-implied constraint, if present, will be added when merging
        // from the pre-entered entry. So we will have both constraints for
        // completeness.
        //
        if (dp.constraint)
          bp.constraints.emplace_back (pdb, name.string (), *dp.constraint);

        // Now collect this prerequisite. If it was actually collected
        // (i.e., it wasn't already there) and we are forcing a downgrade or
        // upgrade, then refuse for a held version, warn for a held package,
        // and print the info message otherwise, unless the verbosity level is
        // less than two.
        //
        // Note though that while the prerequisite was collected it could have
        // happen because it is an optional package and so not being
        // pre-collected earlier. Meanwhile the package was specified
        // explicitly and we shouldn't consider that as a dependency-driven
        // up/down-grade enforcement.
        //
        // Here is an example of the situation we need to handle properly:
        //
        // repo: foo/2(->bar/2), bar/0+1
        // build sys:bar/1
        // build foo ?sys:bar/2
        //
        const build_package* p (
          collect_build (options,
                         move (bp),
                         fdb,
                         rpt_depts,
                         apc,
                         postponed,
                         &dep_chain));

        if (p != nullptr && force && !dep_optional)
        {
          // Fail if the version is held. Otherwise, warn if the package is
          // held.
          //
          bool f (dsp->hold_version);
          bool w (!f && dsp->hold_package);

          if (f || w || verb >= 2)
          {
            const version& av (p->available_version ());

            bool u (av > dsp->version);
            bool c (d.constraint);

            diag_record dr;

            (f ? dr << fail :
             w ? dr << warn :
             dr << info)
              << "package " << name << pdb << " dependency on "
              << (c ? "(" : "") << d << (c ? ")" : "") << " is forcing "
              << (u ? "up" : "down") << "grade of " << *dsp << *ddb << " to ";

            // Print both (old and new) package names in full if the system
            // attribution changes.
            //
            if (dsp->system ())
              dr << p->available_name_version ();
            else
              dr << av; // Can't be a system version so is never wildcard.

            if (dsp->hold_version)
              dr << info << "package version " << *dsp << *ddb << " is held";

            if (f)
              dr << info << "explicitly request version "
                 << (u ? "up" : "down") << "grade to continue";
          }
        }
      }

      dep_chain.pop_back ();
    }

    // Collect the repointed dependents and their replaced prerequisites,
    // recursively.
    //
    // If a repointed dependent is already pre-entered or collected with an
    // action other than adjustment, then just mark it for reconfiguration
    // unless it is already implied. Otherwise, collect the package build with
    // the repoint sub-action and reconfigure adjustment flag.
    //
    void
    collect_repointed_dependents (
      const pkg_build_options& o,
      const repointed_dependents& rpt_depts,
      build_packages::postponed_packages& postponed,
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
        auto rp (make_available (o, db, sp));

        // Add the prerequisite replacements as the required-by packages.
        //
        set<config_package> required_by;
        for (const auto& prq: rd.second)
        {
          if (prq.second) // Prerequisite replacement?
          {
            const config_package& cp (prq.first);
            required_by.emplace (cp.db, cp.name);
          }
        }

        build_package p {
          build_package::build,
          db,
          sp,
          move (rp.first),
          move (rp.second),
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

        collect_build (o,
                       move (p),
                       fdb,
                       rpt_depts,
                       apc,
                       &postponed,
                       &dep_chain);
      }
    }

    // Collect the package being dropped.
    //
    void
    collect_drop (database& db, shared_ptr<selected_package> sp)
    {
      const package_name& nm (sp->name);

      build_package p {
        build_package::drop,
        db,
        move (sp),
        nullptr,
        nullptr,
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

      auto i (map_.find (db, nm));

      if (i != map_.end ())
      {
        build_package& bp (i->second.package);

        // Overwrite the existing (possibly pre-entered, adjustment, or
        // repoint) entry.
        //
        bp = move (p);
      }
      else
        map_.emplace (config_package {db, nm},
                      data_type {end (), move (p)});
    }

    // Collect the package being unheld.
    //
    void
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
          build_package::adjust_unhold};

        p.merge (move (bp));
        bp = move (p);
      }
      else
        bp.flags |= build_package::adjust_unhold;
    }

    void
    collect_build_prerequisites (const pkg_build_options& o,
                                 database& db,
                                 const package_name& name,
                                 postponed_packages& postponed,
                                 const function<find_database_function>& fdb,
                                 const repointed_dependents& rpt_depts,
                                 const function<add_priv_cfg_function>& apc)
    {
      auto mi (map_.find (db, name));
      assert (mi != map_.end ());

      build_package_refs dep_chain;

      collect_build_prerequisites (o,
                                   mi->second.package,
                                   &postponed,
                                   fdb,
                                   rpt_depts,
                                   apc,
                                   dep_chain);
    }

    void
    collect_build_postponed (const pkg_build_options& o,
                             postponed_packages& pkgs,
                             const function<find_database_function>& fdb,
                             const repointed_dependents& rpt_depts,
                             const function<add_priv_cfg_function>& apc)
    {
      // Try collecting postponed packages for as long as we are making
      // progress.
      //
      for (bool prog (true); !pkgs.empty (); )
      {
        postponed_packages npkgs;

        for (const build_package* p: pkgs)
        {
          build_package_refs dep_chain;

          collect_build_prerequisites (o,
                                       *p,
                                       prog ? &npkgs : nullptr,
                                       fdb,
                                       rpt_depts,
                                       apc,
                                       dep_chain);
        }

        assert (prog); // collect_build_prerequisites() should have failed.
        prog = (npkgs != pkgs);
        pkgs.swap (npkgs);
      }
    }

    // Order the previously-collected package with the specified name
    // returning its positions.
    //
    // If buildtime is nullopt, then search for the specified package build in
    // only the specified configuration. Otherwise, treat the package as a
    // dependency and use the custom search function to find its build
    // configuration. Failed that, search for it recursively (see
    // config_package_map::find_dependency() for details).
    //
    // Recursively order the package dependencies being ordered failing if a
    // dependency cycle is detected. If reorder is true, then reorder this
    // package to be considered as "early" as possible.
    //
    iterator
    order (database& db,
           const package_name& name,
           optional<bool> buildtime,
           const function<find_database_function>& fdb,
           bool reorder = true)
    {
      config_package_names chain;
      return order (db, name, buildtime, chain, fdb, reorder);
    }

    // If a configured package is being up/down-graded then that means
    // all its dependents could be affected and we have to reconfigure
    // them. This function examines every package that is already on
    // the list and collects and orders all its dependents. We also need
    // to make sure the dependents are ok with the up/downgrade.
    //
    // Should we reconfigure just the direct depends or also include
    // indirect, recursively? Consider this plauisible scenario as an
    // example: We are upgrading a package to a version that provides
    // an additional API. When its direct dependent gets reconfigured,
    // it notices this new API and exposes its own extra functionality
    // that is based on it. Now it would make sense to let its own
    // dependents (which would be our original package's indirect ones)
    // to also notice this.
    //
    void
    collect_order_dependents (const repointed_dependents& rpt_depts)
    {
      // For each package on the list we want to insert all its dependents
      // before it so that they get configured after the package on which
      // they depend is configured (remember, our build order is reverse,
      // with the last package being built first). This applies to both
      // packages that are already on the list as well as the ones that
      // we add, recursively.
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

    void
    collect_order_dependents (iterator pos,
                              const repointed_dependents& rpt_depts)
    {
      tracer trace ("collect_order_dependents");

      assert (pos != end ());

      build_package& p (*pos);

      database& pdb (p.db);
      const shared_ptr<selected_package>& sp (p.selected);

      const package_name& n (sp->name);

      // See if we are up/downgrading this package. In particular, the
      // available package could be NULL meaning we are just adjusting.
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
          // dependent. But first "prune" if this is a replaced prerequisite
          // of the repointed dependent.
          //
          // Note that the repointed dependents are always collected and have
          // all their collected prerequisites ordered (including new and old
          // ones). See collect_build_prerequisites() and order() for details.
          //
          bool check (ud != 0 && pd.constraint);

          if (i != map_.end () && i->second.position != end ())
          {
            repointed_dependents::const_iterator j (
              rpt_depts.find (config_package {ddb, dn}));

            if (j != rpt_depts.end ())
            {
              const map<config_package, bool>& prereqs_flags (j->second);

              auto k (prereqs_flags.find (config_package {pdb, n}));

              if (k != prereqs_flags.end () && !k->second)
                continue;
            }

            build_package& dp (i->second.package);
            const shared_ptr<selected_package>& dsp (dp.selected);

            // There is one tricky aspect: the dependent could be in the
            // process of being up/downgraded as well. In this case all we
            // need to do is detect this situation and skip the test since all
            // the (new) contraints of this package have been satisfied in
            // collect_build().
            //
            if (check)
            {
              check = dp.available == nullptr ||
                      (dsp->system () == dp.system &&
                       dsp->version == dp.available_version ());
            }
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
                for (const config_package& cp: p.required_by)
                  rb += (rb.empty () ? " " : ", ") + cp.string ();
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

            bool system (dsp->system ()); // Save before the move(dsp) call.

            return build_package {
              build_package::adjust,
                ddb,
                move (dsp),
                nullptr,                   // No available pkg/repo fragment.
                nullptr,
                nullopt,                   // Hold package.
                nullopt,                   // Hold version.
                {},                        // Constraints.
                system,
                false,                     // Keep output directory.
                false,                     // Disfigure (from-scratch reconf).
                false,                     // Configure-only.
                nullopt,                   // Checkout root.
                false,                     // Checkout purge.
                strings (),                // Configuration variables.
                {config_package {pdb, n}}, // Required by (dependency).
                false,                     // Required by dependents.
                build_package::adjust_reconfigure};
          };

          // We can have three cases here: the package is already on the
          // list, the package is in the map (but not on the list) and it
          // is in neither.
          //
          // If the existing entry is a drop, then we skip it. If it is
          // pre-entered, is an adjustment, or is a build that is not supposed
          // to be built (not in the list), then we merge it into the new
          // adjustment entry. Otherwise (is a build in the list), we just add
          // the reconfigure adjustment flag to it.
          //
          if (i != map_.end ())
          {
            build_package& dp (i->second.package);
            iterator& dpos (i->second.position);

            if (!dp.action                         || // Pre-entered.
                *dp.action != build_package::build || // Non-build.
                dpos == end ())                       // Build not in the list.
            {
              // Skip the droped package.
              //
              if (dp.action && *dp.action == build_package::drop)
                continue;

              build_package bp (adjustment ());
              bp.merge (move (dp));
              dp = move (bp);
            }
            else                                       // Build in the list.
              dp.flags |= build_package::adjust_reconfigure;

            // It may happen that the dependent is already in the list but is
            // not properly ordered against its dependencies that get into the
            // list via another dependency path. Thus, we check if the
            // dependent is to the right of its dependency and, if that's the
            // case, reinsert it in front of the dependency.
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
            i = map_.emplace (config_package {ddb, dn},
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

    void
    clear ()
    {
      build_package_list::clear ();
      map_.clear ();
    }

    void
    clear_order ()
    {
      build_package_list::clear ();

      for (auto& p: map_)
        p.second.position = end ();
    }

  private:
    struct config_package_name
    {
      database& db;
      const package_name& name;

      bool
      operator== (const config_package_name& v)
      {
        return name == v.name && db == v.db;
      }
    };
    using config_package_names = small_vector<config_package_name, 16>;

    iterator
    order (database& db,
           const package_name& name,
           optional<bool> buildtime,
           config_package_names& chain,
           const function<find_database_function>& fdb,
           bool reorder)
    {
      config_package_map::iterator mi;

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
      config_package_name cp {pdb, name};
      {
        auto i (find (chain.begin (), chain.end (), cp));

        if (i != chain.end ())
        {
          diag_record dr (fail);
          dr << "dependency cycle detected involving package " << name << pdb;

          auto nv = [this] (const config_package_name& cp)
          {
            auto mi (map_.find (cp.db, cp.name));
            assert (mi != map_.end ());

            build_package& p (mi->second.package);

            assert (p.action); // See above.

            // We cannot end up with a dependency cycle for actions other than
            // build since these packages are configured and we would fail on
            // a previous run while building them.
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

      // If this package is already in the list, then that would also
      // mean all its prerequisites are in the list and we can just
      // return its position. Unless we want it reordered.
      //
      iterator& pos (mi->second.position);
      if (pos != end ())
      {
        if (reorder)
          erase (pos);
        else
          return pos;
      }

      // Order all the prerequisites of this package and compute the
      // position of its "earliest" prerequisite -- this is where it
      // will be inserted.
      //
      const shared_ptr<selected_package>&  sp (p.selected);
      const shared_ptr<available_package>& ap (p.available);

      bool build (*p.action == build_package::build);

      // Package build must always have the available package associated.
      //
      assert (!build || ap != nullptr);

      // Unless this package needs something to be before it, add it to
      // the end of the list.
      //
      iterator i (end ());

      // Figure out if j is before i, in which case set i to j. The goal
      // here is to find the position of our "earliest" prerequisite.
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
      // And yet, if the package we are ordering is a repointed dependent,
      // then we must order not only its unamended and new prerequisites but
      // also its replaced prerequisites, which can also be disfigured.
      //
      bool src_conf (sp != nullptr &&
                     sp->state == package_state::configured &&
                     sp->substate != package_substate::system);

      auto disfigure = [] (const build_package& p)
      {
        return p.action && (*p.action == build_package::drop ||
                            p.reconfigure ());
      };

      bool order_disfigured (src_conf && disfigure (p));

      chain.push_back (cp);

      // Order the build dependencies.
      //
      if (build && !p.system)
      {
        // So here we are going to do things differently depending on
        // whether the package is already configured or not. If it is and
        // not as a system package, then that means we can use its
        // prerequisites list. Otherwise, we use the manifest data.
        //
        if (src_conf && sp->version == p.available_version ())
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
          // We are iterating in reverse so that when we iterate over
          // the dependency list (also in reverse), prerequisites will
          // be built in the order that is as close to the manifest as
          // possible.
          //
          for (const dependency_alternatives_ex& da:
                 reverse_iterate (ap->dependencies))
          {
            assert (!da.conditional && da.size () == 1); // @@ TODO
            const dependency& d (da.front ());
            const package_name& dn (d.name);

            // Skip special names.
            //
            if (da.buildtime && (dn == "build2" || dn == "bpkg"))
              continue;

            // Note that for the repointed dependent we only order its new and
            // unamended prerequisites here. Its replaced prerequisites will
            // be ordered below.
            //
            update (order (pdb,
                           d.name,
                           da.buildtime,
                           chain,
                           fdb,
                           false /* reorder */));
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

  private:
    struct data_type
    {
      iterator position;         // Note: can be end(), see collect_build().
      build_package package;
    };

    class config_package_map: public map<config_package, data_type>
    {
    public:
      using base_type = map<config_package, data_type>;

      iterator
      find (database& db, const package_name& pn)
      {
        return base_type::find (config_package {db, pn});
      }

      // Try to find a package build in the dependency configurations (see
      // database::dependency_configs() for details). Return the end iterator
      // if no build is found and issue diagnostics and fail if multiple
      // builds (in multiple configurations) are found.
      //
      iterator
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
                info << r->first.db.config_orig <<
                info << ldb.config_orig <<
                info << "use --config-* to select package configuration";
          }
        }

        return r;
      }
    };
    config_package_map map_;
  };

  // Return a patch version constraint for the selected package if it has a
  // standard version, otherwise, if requested, issue a warning and return
  // nullopt.
  //
  // Note that the function may also issue a warning and return nullopt if the
  // selected package minor version reached the limit (see
  // standard-version.cxx for details).
  //
  static optional<version_constraint>
  patch_constraint (const shared_ptr<selected_package>& sp, bool quiet = false)
  {
    const package_name& nm (sp->name);
    const version&      sv (sp->version);

    // Note that we don't pass allow_stub flag so the system wildcard version
    // will (naturally) not be patched.
    //
    string vs (sv.string ());
    optional<standard_version> v (parse_standard_version (vs));

    if (!v)
    {
      if (!quiet)
        warn << "unable to patch " << package_string (nm, sv) <<
          info << "package is not using semantic/standard version";

      return nullopt;
    }

    try
    {
      return version_constraint ("~" + vs);
    }
    // Note that the only possible reason for invalid_argument exception to
    // be thrown is that minor version reached the 99999 limit (see
    // standard-version.cxx for details).
    //
    catch (const invalid_argument&)
    {
      if (!quiet)
        warn << "unable to patch " << package_string (nm, sv) <<
          info << "minor version limit reached";

      return nullopt;
    }
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
    database*                    db;             // Can only be NULL if system.
    package_name                 name;
    optional<version_constraint> constraint;     // nullopt if unspecified.
    shared_ptr<selected_package> selected;
    bool                         system;
    bool                         patch;          // Only for an empty version.
    bool                         keep_out;
    bool                         disfigure;
    optional<dir_path>           checkout_root;
    bool                         checkout_purge;
    strings                      config_vars;    // Only if not system.
  };
  using dependency_packages = vector<dependency_package>;

  // Evaluate a dependency package and return a new desired version. If the
  // result is absent (nullopt), then there are no user expectations regarding
  // this dependency. If the result is a NULL available_package, then it is
  // either no longer used and can be dropped, or no changes to the dependency
  // are necessary. Otherwise, the result is available_package to
  // upgrade/downgrade to as well as the repository fragment it must come
  // from, and the system flag.
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
    // The system flag is meaningless if the unused flag is true.
    //
    reference_wrapper<database>                db;
    shared_ptr<available_package>              available;
    lazy_shared_ptr<bpkg::repository_fragment> repository_fragment;
    bool                                       unused;
    bool                                       system;
  };

  struct config_package_dependent
  {
    database&                    db;
    shared_ptr<selected_package> package;
    optional<version_constraint> constraint;

    config_package_dependent (database& d,
                              shared_ptr<selected_package> p,
                              optional<version_constraint> c)
        : db (d), package (move (p)), constraint (move (c)) {}
  };

  using config_package_dependents = vector<config_package_dependent>;

  static optional<evaluate_result>
  evaluate_dependency (database&,
                       const shared_ptr<selected_package>&,
                       const optional<version_constraint>& desired,
                       bool desired_sys,
                       database& desired_db,
                       const shared_ptr<selected_package>& desired_db_sp,
                       bool patch,
                       bool explicitly,
                       const config_repo_fragments&,
                       const config_package_dependents&,
                       bool ignore_unsatisfiable);

  // If there are no user expectations regarding this dependency, then we give
  // no up/down-grade recommendation, unless there are no dependents in which
  // case we recommend to drop the dependency.
  //
  // Note that the user expectations are only applied for dependencies that
  // have dependents in the current configurations.
  //
  static optional<evaluate_result>
  evaluate_dependency (database& db,
                       const shared_ptr<selected_package>& sp,
                       const dependency_packages& deps,
                       bool no_move,
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
                              false   /* system */};
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
                info << j->db->config_orig;
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
                              false   /* system */};
    }

    // The requested dependency database, version constraint, and system flag.
    //
    assert (i != deps.end ());

    database& ddb (i->db != nullptr ? *i->db : db);
    const optional<version_constraint>& dvc (i->constraint); // May be nullopt.
    bool dsys (i->system);

    // The selected package in the desired database which we copy over.
    //
    // It is the current dependency package, if we don't copy, and may or may
    // not exist otherwise.
    //
    shared_ptr<selected_package> dsp (db == ddb
                                      ? sp
                                      : ddb.find<selected_package> (nm));

    // If a package in the desired database is already selected and matches
    // the user expectations then no package change is required.
    //
    if (dsp != nullptr && dvc)
    {
      const version& sv (dsp->version);
      bool ssys (dsp->system ());

      if (ssys == dsys &&
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
    config_package_dependents dependents;

    for (auto& pd: pds)
    {
      database& ddb (pd.first);
      package_dependent& dep (pd.second);

      shared_ptr<selected_package> p (ddb.load<selected_package> (dep.name));

      add_dependent_repo_fragments (
        ddb,
        available_package_id (p->name, p->version),
        repo_frags);

      dependents.emplace_back (ddb, move (p), move (dep.constraint));
    }

    return evaluate_dependency (db,
                                sp,
                                dvc,
                                dsys,
                                ddb,
                                dsp,
                                i->patch,
                                true /* explicitly */,
                                repo_frags,
                                dependents,
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

  static optional<evaluate_result>
  evaluate_dependency (database& db,
                       const shared_ptr<selected_package>& sp,
                       const optional<version_constraint>& dvc,
                       bool dsys,
                       database& ddb,
                       const shared_ptr<selected_package>& dsp,
                       bool patch,
                       bool explicitly,
                       const config_repo_fragments& rfs,
                       const config_package_dependents& dependents,
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
                              false   /* system */};
    };

    // Build the list of available packages for the potential up/down-grade
    // to, in the version-descending order. If patching, then we constrain the
    // choice with the latest patch version and place no constraints if
    // upgrading. For a system package we also put no constraints just to make
    // sure that the package is recognized.
    //
    optional<version_constraint> c;

    if (!dvc)
    {
      assert (!dsys); // The version can't be empty for the system package.

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
    else if (!dsys)
      c = dvc;

    vector<pair<shared_ptr<available_package>,
                lazy_shared_ptr<repository_fragment>>> afs (
      find_available (nm, c, rfs));

    // Go through up/down-grade candidates and pick the first one that
    // satisfies all the dependents. Collect (and sort) unsatisfied dependents
    // per the unsatisfiable version in case we need to print them.
    //
    using sp_set = set<config_selected_package>;

    vector<pair<version, sp_set>> unsatisfiable;

    bool stub (false);

    assert (!dsys ||
            (ddb.system_repository &&
             ddb.system_repository->find (nm) != nullptr));

    for (auto& af: afs)
    {
      shared_ptr<available_package>& ap (af.first);
      const version& av (!dsys ? ap->version : *ap->system_version (ddb));

      // If we aim to upgrade to the latest version and it tends to be less
      // then the selected one, then what we currently have is the best that
      // we can get, and so we return the "no change" result.
      //
      // Note that we also handle a package stub here.
      //
      if (!dvc && dsp != nullptr && av < dsp->version)
      {
        assert (!dsys); // Version can't be empty for the system package.

        // For the selected system package we still need to pick a source
        // package version to downgrade to.
        //
        if (!dsp->system ())
        {
          l5 ([&]{trace << *dsp << ddb << ": best";});
          return no_change ();
        }

        // We can not upgrade the (system) package to a stub version, so just
        // skip it.
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

      for (const auto& dp: dependents)
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

      // If the best satisfactory version and the desired system flag perfectly
      // match the ones of the selected package, then no package change is
      // required. Otherwise, recommend an up/down-grade.
      //
      if (dsp != nullptr && av == dsp->version && dsp->system () == dsys)
      {
        l5 ([&]{trace << *dsp << ddb << ": unchanged";});
        return no_change ();
      }

      l5 ([&]{trace << *sp << db << ": update to "
                    << package_string (nm, av, dsys) << ddb;});

      return evaluate_result {
        ddb, move (ap), move (af.second), false /* unused */, dsys};
    }

    // If we aim to upgrade to the latest version, then what we currently have
    // is the only thing that we can get, and so returning the "no change"
    // result, unless we need to upgrade a package configured as system.
    //
    if (!dvc && dsp != nullptr && !dsp->system ())
    {
      assert (!dsys); // Version cannot be empty for the system package.

      l5 ([&]{trace << *dsp << ddb << ": only";});
      return no_change ();
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

      if (!dvc && patch)
      {
        // Otherwise, we should have bailed out earlier (see above).
        //
        assert (dsp != nullptr && dsp->system ());

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
        // Note that we don't advise to "build" the package as a system one as
        // it is already as such (see above).
        //
        assert (!dvc && !dsys && dsp != nullptr && dsp->system ());

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
  // upgraded (specified with -i/-r on the command line).
  //
  struct recursive_package
  {
    database&    db;
    package_name name;
    bool         upgrade;   // true -- upgrade,   false -- patch.
    bool         recursive; // true -- recursive, false -- immediate.
  };
  using recursive_packages = vector<recursive_package>;

  // Recursively check if immediate dependencies of this dependent must be
  // upgraded or patched. Return true if it must be upgraded, false if
  // patched, and nullopt otherwise.
  //
  static optional<bool>
  upgrade_dependencies (database& db,
                        const package_name& nm,
                        const recursive_packages& rs,
                        bool recursion = false)
  {
    auto i (find_if (rs.begin (), rs.end (),
                     [&nm, &db] (const recursive_package& i) -> bool
                     {
                       return i.name == nm && i.db == db;
                     }));

    optional<bool> r;

    if (i != rs.end () && i->recursive >= recursion)
    {
      r = i->upgrade;

      if (*r) // Upgrade (vs patch)?
        return r;
    }

    for (database& ddb: db.dependent_configs ())
    {
      for (auto& pd: query_dependents_cache (ddb, nm, db))
      {
        // Note that we cannot end up with an infinite recursion for
        // configured packages due to a dependency cycle (see order() for
        // details).
        //
        if (optional<bool> u = upgrade_dependencies (ddb, pd.name, rs, true))
        {
          if (!r || *r < *u) // Upgrade wins patch.
          {
            r = u;

            if (*r) // Upgrade (vs patch)?
              return r;
          }
        }
      }
    }

    return r;
  }

  // Evaluate a package (not necessarily dependency) and return a new desired
  // version. If the result is absent (nullopt), then no changes to the
  // package are necessary. Otherwise, the result is available_package to
  // upgrade/downgrade to as well as the repository fragment it must come
  // from.
  //
  // If the system package cannot be upgraded to the source one, not being
  // found in the dependents repositories, then return nullopt if
  // ignore_unsatisfiable argument is true and fail otherwise (see the
  // evaluate_dependency() function description for details).
  //
  static optional<evaluate_result>
  evaluate_recursive (database& db,
                      const shared_ptr<selected_package>& sp,
                      const recursive_packages& recs,
                      bool ignore_unsatisfiable)
  {
    tracer trace ("evaluate_recursive");

    assert (sp != nullptr);

    // Build a set of repository fragment the dependent packages come from.
    // Also cache the dependents and the constraints they apply to this
    // dependency.
    //
    config_repo_fragments repo_frags;
    config_package_dependents dependents;

    // Only collect repository fragments (for best version selection) of
    // (immediate) dependents that have a hit (direct or indirect) in recs.
    // Note, however, that we collect constraints from all the dependents.
    //
    optional<bool> upgrade;

    for (database& ddb: db.dependent_configs ())
    {
      for (auto& pd: query_dependents_cache (ddb, sp->name, db))
      {
        shared_ptr<selected_package> p (ddb.load<selected_package> (pd.name));

        dependents.emplace_back (ddb, p, move (pd.constraint));

        if (optional<bool> u = upgrade_dependencies (ddb, pd.name, recs))
        {
          if (!upgrade || *upgrade < *u) // Upgrade wins patch.
            upgrade = u;
        }
        else
          continue;

        // While we already know that the dependency upgrade is required, we
        // continue to iterate over dependents, collecting the repository
        // fragments and the constraints.
        //
        add_dependent_repo_fragments (
          ddb,
          available_package_id (p->name, p->version),
          repo_frags);
      }
    }

    if (!upgrade)
    {
      l5 ([&]{trace << *sp << db << ": no hit";});
      return nullopt;
    }

    // Recommends the highest possible version.
    //
    optional<evaluate_result> r (
      evaluate_dependency (db,
                           sp,
                           nullopt /* desired */,
                           false /*desired_sys */,
                           db,
                           sp,
                           !*upgrade /* patch */,
                           false /* explicitly */,
                           repo_frags,
                           dependents,
                           ignore_unsatisfiable));

    // Translate the "no change" result into nullopt.
    //
    assert (!r || !r->unused);
    return r && r->available == nullptr ? nullopt : r;
  }

  // Return false if the plan execution was noop.
  //
  static bool
  execute_plan (const pkg_build_options&,
                build_package_list&,
                bool simulate,
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
    // explicit --upgrade or --patch.
    //
    if (const char* n = (o.immediate () ? "--immediate" :
                         o.recursive () ? "--recursive" : nullptr))
    {
      if (!o.upgrade () && !o.patch ())
        dr << fail << n << " requires explicit --upgrade|-u or --patch|-p";
    }

    if (((o.upgrade_immediate () ? 1 : 0) +
         (o.upgrade_recursive () ? 1 : 0) +
         (o.patch_immediate ()   ? 1 : 0) +
         (o.patch_recursive ()   ? 1 : 0)) > 1)
      dr << fail << "multiple --(upgrade|patch)-(immediate|recursive) "
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
      // -u|-p.
      //
      if (!(dst.upgrade () || dst.patch ()))
      {
        dst.upgrade (src.upgrade ());
        dst.patch   (src.patch ());
      }
    }

    if (!(dst.upgrade_immediate () || dst.upgrade_recursive () ||
          dst.patch_immediate ()   || dst.patch_recursive ()))
    {
      dst.upgrade_immediate (src.upgrade_immediate ());
      dst.upgrade_recursive (src.upgrade_recursive ());
      dst.patch_immediate   (src.patch_immediate ());
      dst.patch_recursive   (src.patch_recursive ());
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
    return x.keep_out ()          == y.keep_out ()          &&
           x.disfigure ()         == y.disfigure ()         &&
           x.dependency ()        == y.dependency ()        &&
           x.upgrade ()           == y.upgrade ()           &&
           x.patch ()             == y.patch ()             &&
           x.immediate ()         == y.immediate ()         &&
           x.recursive ()         == y.recursive ()         &&
           x.upgrade_immediate () == y.upgrade_immediate () &&
           x.upgrade_recursive () == y.upgrade_recursive () &&
           x.patch_immediate ()   == y.patch_immediate ()   &&
           x.patch_recursive ()   == y.patch_recursive ()   &&
           x.checkout_root ()     == y.checkout_root ()     &&
           x.checkout_purge ()    == y.checkout_purge ();
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

    if (!args.more () && !o.upgrade () && !o.patch ())
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
    }

    validate_options (o, ""); // Global package options.

    // Note that the session spans all our transactions. The idea here is that
    // selected_package objects in build_packages below will be cached in this
    // session. When subsequent transactions modify any of these objects, they
    // will modify the cached instance, which means our list will always "see"
    // their updated state.
    //
    // Also note that rep_fetch() must be called in session.
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

        cvars.push_back (move (v));
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

        add_bool ("--keep-out",          o.keep_out ());
        add_bool ("--disfigure",         o.disfigure ());
        add_bool ("--upgrade",           o.upgrade ());
        add_bool ("--patch",             o.patch ());
        add_bool ("--immediate",         o.immediate ());
        add_bool ("--recursive",         o.recursive ());
        add_bool ("--upgrade-immediate", o.upgrade_immediate ());
        add_bool ("--upgrade-recursive", o.upgrade_recursive ());
        add_bool ("--patch-immediate",   o.patch_immediate ());
        add_bool ("--patch-recursive",   o.patch_recursive ());

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

    // Add the system package authoritative information to the database's
    // system repository, unless it already contains authoritative information
    // for this package.
    //
    // Note that it is assumed that all the possible duplicates are handled
    // elsewhere/later.
    //
    auto add_system_package = [] (database& db,
                                  const package_name& nm,
                                  const version& v)
    {
      assert (db.system_repository);

      const system_package* sp (db.system_repository->find (nm));

      if (sp == nullptr || !sp->authoritative)
        db.system_repository->insert (nm, v, true /* authoritative */);
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
                        strings vs) -> pkg_arg
    {
      assert (!vc || !vc->empty ()); // May not be empty if present.

      if (db == nullptr)
        assert (sc == package_scheme::sys && os.dependency ());

      pkg_arg r {
        db, sc, move (nm), move (vc), string (), move (os), move (vs)};

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
          if (!r.constraint)
            r.constraint = version_constraint (wildcard_version);

          // The system package may only have an exact/wildcard version
          // specified.
          //
          assert (r.constraint->min_version == r.constraint->max_version);

          if (db != nullptr)
            add_system_package (*db, r.name, *r.constraint->min_version);

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
                      move (vs)};
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
      auto fold_zero_rev = [] (package_scheme sc)
      {
        bool r (false);
        switch (sc)
        {
        case package_scheme::none: r = false; break;
        case package_scheme::sys:  r = true;  break;
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
                s, sys, fold_zero_rev (sc), version_only (sc)));

            // For system packages not associated with a specific repository
            // location add the stub package to the imaginary system
            // repository (see above for details).
            //
            if (sys && vc)
              stubs.push_back (make_shared<available_package> (n));

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
                                             move (ps.config_vars)));
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
                s, sys, fold_zero_rev (sc), version_only (sc)));

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
                                             ps.config_vars));
          }
        }
      }

      t.commit ();

      imaginary_stubs = move (stubs);
    }

    // List of package configurations specified on the command line.
    //
    vector<config_package> pkg_confs;

    // Separate the packages specified on the command line into to hold and to
    // up/down-grade as dependencies, and save dependents whose dependencies
    // must be upgraded recursively.
    //
    vector<build_package> hold_pkgs;
    dependency_packages   dep_pkgs;
    recursive_packages    rec_pkgs;

    {
      // Check if the package is a duplicate. Return true if it is but
      // harmless.
      //
      struct config_package_key // Like config_package but with NULL'able db.
      {
        package_name name;
        database*    db;   // Can be NULL for system dependency.

        config_package_key (package_name n, database* d)
            : name (move (n)), db (d) {}

        bool
        operator< (const config_package_key& v) const
        {
          if (int r = name.compare (v.name))
            return r < 0;

          return db != nullptr && v.db != nullptr ? *db < *v.db :
                 db == nullptr && v.db == nullptr ? false       :
                                                    db == nullptr;
        }
      };

      map<config_package_key, pkg_arg> package_map;

      auto check_dup = [&package_map, &arg_string, &arg_parsed]
                       (const pkg_arg& pa) -> bool
      {
        assert (arg_parsed (pa));

        auto r (package_map.emplace (config_package_key {pa.name, pa.db}, pa));

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
            info << "first mentioned as " << arg_string (r.first->second) <<
            info << "second mentioned as " << arg_string (pa);

        return !r.second;
      };

      transaction t (mdb);

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

        if (!arg_parsed (pa))
        {
          assert (pdb != nullptr); // Unparsed and so can't be system.

          lazy_shared_ptr<repository_fragment> root (*pdb, empty_string);

          const char* package (pa.value.c_str ());

          // Is this a package archive?
          //
          bool package_arc (false);

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
                            false /* expand_values */,
                            true /* complete_depends */,
                            diag));

              // This is a package archive.
              //
              // Note that throwing failed from here on will be fatal.
              //
              package_arc = true;

              l4 ([&]{trace << "archive '" << a << "': " << arg_string (pa);});

              // Supporting this would complicate things a bit, but we may add
              // support for it one day.
              //
              if (pa.options.dependency ())
                fail << "package archive '" << a
                     << "' may not be built as a dependency";

              pa = arg_package (pdb,
                                package_scheme::none,
                                m.name,
                                version_constraint (m.version),
                                move (pa.options),
                                move (pa.config_vars));

              af = root;
              ap = make_shared<available_package> (move (m));
              ap->locations.push_back (package_location {root, move (a)});
            }
          }
          catch (const invalid_path&)
          {
            // Not a valid path so cannot be an archive.
          }
          catch (const failed& e)
          {
            // If this is a valid package archive but something went wrong
            // afterwards, then we are done.
            //
            if (package_arc)
              throw;

            assert (e.code == 1);
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
            bool package_dir (false);

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
                    d,
                    true /* ignore_unknown */,
                    [&o, &d, &pvi] (version& v)
                    {
                      pvi = package_version (o, d);

                      if (pvi.version)
                        v = move (*pvi.version);
                    },
                    diag));

                // This is a package directory.
                //
                // Note that throwing failed from here on will be fatal.
                //
                package_dir = true;

                l4 ([&]{trace << "directory '" << d << "': "
                              << arg_string (pa);});

                // Supporting this would complicate things a bit, but we may
                // add support for it one day.
                //
                if (pa.options.dependency ())
                  fail << "package directory '" << d
                       << "' may not be built as a dependency";

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
              }
            }
            catch (const invalid_path&)
            {
              // Not a valid path so cannot be a package directory.
            }
            catch (const failed& e)
            {
              // If this is a valid package directory but something went wrong
              // afterwards, then we are done.
              //
              if (package_dir)
                throw;

              assert (e.code == 1);
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
                  false /* fold_zero_revision */));

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

              // Either get the user-specified version or the latest allowed
              // for a source code package. For a system package we pick the
              // latest one just to make sure the package is recognized.
              //
              optional<version_constraint> c;

              if (!pa.constraint)
              {
                assert (!arg_sys (pa));

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
              else if (!arg_sys (pa))
                c = pa.constraint;

              auto rp (find_available_one (pa.name, c, root));

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
        // recursive upgrade.
        //
        {
          optional<bool> u;
          optional<bool> r;

          const auto& po (pa.options);

          if      (po.upgrade_immediate ()) { u = true;          r = false; }
          else if (po.upgrade_recursive ()) { u = true;          r = true;  }
          else if (  po.patch_immediate ()) { u = false;         r = false; }
          else if (  po.patch_recursive ()) { u = false;         r = true;  }
          else if (        po.immediate ()) { u = po.upgrade (); r = false; }
          else if (        po.recursive ()) { u = po.upgrade (); r = true;  }

          if (r)
          {
            l4 ([&]{trace << "stashing recursive package "
                          << arg_string (pa);});

            // The above options are meaningless for system packages, so we
            // just ignore them for a system dependency with unspecified
            // configuration.
            //
            if (pdb != nullptr)
              rec_pkgs.push_back (recursive_package {*pdb, pa.name, *u, *r});
          }
        }

        // Add the dependency package to the list.
        //
        if (pa.options.dependency ())
        {
          l4 ([&]{trace << "stashing dependency package "
                        << arg_string (pa);});

          bool sys (arg_sys (pa));

          // Make sure that the package is known.
          //
          auto apr (find_available (repo_configs,
                                    pa.name,
                                    !sys ? pa.constraint : nullopt));

          if (apr.empty ())
          {
            diag_record dr (fail);
            dr << "unknown package " << arg_string (pa, false /* options */);
            check_any_available (repo_configs, t, &dr);
          }

          if (pdb != nullptr)
          {
            // Save before the name move.
            //
            sp = pdb->find<selected_package> (pa.name);

            pkg_confs.emplace_back (*pdb, pa.name);
          }

          dep_pkgs.push_back (
            dependency_package {pdb,
                                move (pa.name),
                                move (pa.constraint),
                                move (sp),
                                sys,
                                pa.options.patch (),
                                pa.options.keep_out (),
                                pa.options.disfigure (),
                                (pa.options.checkout_root_specified ()
                                 ? move (pa.options.checkout_root ())
                                 : optional<dir_path> ()),
                                pa.options.checkout_purge (),
                                move (pa.config_vars)});
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

        // If the package is not available from the repository we can try to
        // create it from the orphaned selected package. Meanwhile that
        // doesn't make sense for a system package. The only purpose to
        // configure a system package is to build its dependent. But if the
        // package is not in the repository then there is no dependent for it
        // (otherwise the repository would be broken).
        //
        if (!arg_sys (pa))
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
            for (;;)
            {
              if (ap != nullptr) // Must be that version, see above.
                break;

              // Otherwise, our only chance is that the already selected object
              // satisfies the version constraint.
              //
              if (sp != nullptr  &&
                  !sp->system () &&
                  satisfies (sp->version, pa.constraint))
                break; // Derive ap from sp below.

              found = false;
              break;
            }
          }
          //
          // No explicit version was specified by the user (not relevant for a
          // system package, see above).
          //
          else
          {
            assert (!arg_sys (pa));

            if (ap != nullptr)
            {
              assert (!ap->stub ());

              // Even if this package is already in the configuration, should
              // we have a newer version, we treat it as an upgrade request;
              // otherwise, why specify the package in the first place? We just
              // need to check if what we already have is "better" (i.e.,
              // newer).
              //
              if (sp != nullptr && !sp->system () && ap->version < sp->version)
                ap = nullptr; // Derive ap from sp below.
            }
            else
            {
              if (sp == nullptr || sp->system ())
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
            dr << "unknown package " << pa.name;

            // Let's help the new user out here a bit.
            //
            check_any_available (*pdb, t, &dr);
          }
          else
          {
            assert (!arg_sys (pa));

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
          assert (sp != nullptr && sp->system () == arg_sys (pa));

          auto rp (make_available (o, *pdb, sp));
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

        // Finally add this package to the list.
        //
        // @@ Pass pa.configure_only() when support for package-specific
        //    --configure-only is added.
        //
        build_package p {
          build_package::build,
          *pdb,
          move (sp),
          move (ap),
          move (af),
          true,                       // Hold package.
          pa.constraint.has_value (), // Hold version.
          {},                         // Constraints.
          arg_sys (pa),
          keep_out,
          pa.options.disfigure (),
          false,                      // Configure-only.
          (pa.options.checkout_root_specified ()
           ? move (pa.options.checkout_root ())
           : optional<dir_path> ()),
          pa.options.checkout_purge (),
          move (pa.config_vars),
          {config_package {mdb, ""}}, // Required by (command line).
          false,                      // Required by dependents.
          0};                         // State flags.

        l4 ([&]{trace << "stashing held package "
                      << p.available_name_version_db ();});

        // "Fix" the version the user asked for by adding the constraint.
        //
        // Note: for a system package this must always be present (so that
        // this build_package instance is never replaced).
        //
        if (pa.constraint)
          p.constraints.emplace_back (
            mdb, "command line", move (*pa.constraint));

        pkg_confs.emplace_back (p.db, p.name ());

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
          (o.upgrade () || o.patch ()))
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

            auto apr (find_available_one (name, pc, root));

            shared_ptr<available_package> ap (move (apr.first));
            if (ap == nullptr || ap->stub ())
            {
              diag_record dr (fail);
              dr << name << " is not available";

              if (ap != nullptr)
                dr << " in source" <<
                  info << "consider building it as "
                   << package_string (name, version (), true /* system */)
                   << " if it is available from the system";

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
                true,                       // Hold package.
                false,                      // Hold version.
                {},                         // Constraints.
                false,                      // System package.
                keep_out,
                o.disfigure (),
                false,                      // Configure-only.
                nullopt,                    // Checkout root.
                false,                      // Checkout purge.
                strings (),                 // Configuration variables.
                {config_package {mdb, ""}}, // Required by (command line).
                false,                      // Required by dependents.
                0};                         // State flags.

            l4 ([&]{trace << "stashing held package "
                          << p.available_name_version_db ();});

            hold_pkgs.push_back (move (p));

            // If there are also -i|-r, then we are also upgrading dependencies
            // of all held packages.
            //
            if (o.immediate () || o.recursive ())
              rec_pkgs.push_back (
                recursive_package {cdb, name, o.upgrade (), o.recursive ()});
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

        for (const config_package& cp: pkg_confs)
        {
          if (cp.name == nm &&
              find (ddbs.begin (), ddbs.end (), cp.db) != ddbs.end ())
          {
            if (r == nullptr)
              r = &cp.db;
            else
              fail << "multiple " << cp.db.type << " configurations "
                   << "specified for package " << nm <<
                info << r->config_orig <<
                info << cp.db.config_orig;
          }
        }

        return r;
      });

    // Assemble the list of packages we will need to build-to-hold, still used
    // dependencies to up/down-grade, and unused dependencies to drop. We call
    // this the plan.
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

        bool system;
      };
      vector<dep> deps;

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
            map<config_package, bool> ps; // Old/new prerequisites.

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
                ps.emplace (config_package {*pdb, name}, true);
                ps.emplace (config_package {  db, name}, false);
              }
            }

            if (!ps.empty ())
              rpt_depts.emplace (config_package {cdb, sp->name}, move (ps));
          }
        }

        t.commit ();
      }

      // Iteratively refine the plan with dependency up/down-grades/drops.
      //
      for (bool refine (true), scratch (true); refine; )
      {
        l4 ([&]{trace << "refining execution plan"
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
        // Note that we don't copy the prerequisite constraints into the
        // replacements, since they are unused in the collecting/ordering
        // logic.
        //
        for (auto& rd: rpt_depts)
        {
          database&           db (rd.first.db);
          const package_name& nm (rd.first.name);

          shared_ptr<selected_package> sp (db.load<selected_package> (nm));

          for (const auto& prq: rd.second)
          {
            if (prq.second) // Prerequisite replacement?
            {
              const config_package& cp (prq.first);

              auto i (sp->prerequisites.emplace (
                        lazy_shared_ptr<selected_package> (cp.db, cp.name),
                        nullopt));

              // The selected package should only contain the old
              // prerequisites at this time, so adding a replacement should
              // always succeed.
              //
              assert (i.second);
            }
          }

          db.update (sp);
        }

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
        auto enter = [&mdb, &pkgs] (database& db,
                                    const dependency_package& p)
        {
          build_package bp {
            nullopt,                    // Action.
            db,
            nullptr,                    // Selected package.
            nullptr,                    // Available package/repo fragment.
            nullptr,
            false,                      // Hold package.
            p.constraint.has_value (),  // Hold version.
            {},                         // Constraints.
            p.system,
            p.keep_out,
            p.disfigure,
            false,                      // Configure-only.
            p.checkout_root,
            p.checkout_purge,
            p.config_vars,
            {config_package {mdb, ""}}, // Required by (command line).
            false,                      // Required by dependents.
            0};                         // State flags.

          if (p.constraint)
            bp.constraints.emplace_back (
              mdb, "command line", *p.constraint);

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
          add_system_package (db,
                              p.name,
                              (p.constraint
                               ? *p.constraint->min_version
                               : wildcard_version));

          enter (db, p);
        };

        // Private configurations that were created during collection of the
        // package builds.
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
        private_configs priv_cfgs;

        // Add a newly created private configuration to the private
        // configurations and the dependency databases lists and pre-enter
        // builds of system dependencies with unspecified configuration for
        // this configuration.
        //
        const function<add_priv_cfg_function> add_priv_cfg (
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

        build_packages::postponed_packages postponed;

        if (scratch)
        {
          pkgs.clear ();

          // Pre-enter dependencies with specified configurations.
          //
          for (const dependency_package& p: dep_pkgs)
          {
            if (p.db != nullptr)
              enter (*p.db, p);
          }

          // Pre-enter system dependencies with unspecified configuration for
          // all dependency configurations, excluding those which already have
          // this dependency pre-entered.
          //
          for (const dependency_package& p: dep_pkgs)
          {
            if (p.db == nullptr)
            {
              for (database& db: dep_dbs)
              {
                if (!pkgs.entered (db, p.name))
                  enter_system_dependency (db, p);
              }
            }
          }

          // Pre-collect user selection to make sure dependency-forced
          // up/down-grades are handled properly (i.e., the order in which we
          // specify packages on the command line does not matter).
          //
          for (const build_package& p: hold_pkgs)
            pkgs.collect_build (o,
                                p,
                                find_prereq_database,
                                rpt_depts,
                                add_priv_cfg);

          // Collect all the prerequisites of the user selection.
          //
          for (const build_package& p: hold_pkgs)
            pkgs.collect_build_prerequisites (o,
                                              p.db,
                                              p.name (),
                                              postponed,
                                              find_prereq_database,
                                              rpt_depts,
                                              add_priv_cfg);

          // Note that we need to collect unheld after prerequisites, not to
          // overwrite the pre-entered entries before they are used to provide
          // additional constraints for the collected prerequisites.
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
                                             postponed,
                                             find_prereq_database,
                                             add_priv_cfg);

          scratch = false;
        }
        else
          pkgs.clear_order (); // Only clear the ordered list.

        // Add to the plan dependencies to up/down-grade/drop that were
        // discovered on the previous iterations.
        //
        for (const dep& d: deps)
        {
          database& ddb (d.db);

          if (d.available == nullptr)
          {
            pkgs.collect_drop (ddb, ddb.load<selected_package> (d.name));
          }
          else
          {
            shared_ptr<selected_package> sp (
              ddb.find<selected_package> (d.name));

            // We will keep the output directory only if the external package
            // is replaced with an external one (see above for details).
            //
            bool keep_out (o.keep_out () && sp->external ());

            // Marking upgraded dependencies as "required by command line" may
            // seem redundant as they should already be pre-entered as such
            // (see above). But remember dependencies upgraded with -i|-r?
            // Note that the required_by data member should never be empty, as
            // it is used in prompts/diagnostics.
            //
            build_package p {
              build_package::build,
              ddb,
              move (sp),
              d.available,
              d.repository_fragment,
              nullopt,                    // Hold package.
              nullopt,                    // Hold version.
              {},                         // Constraints.
              d.system,
              keep_out,
              o.disfigure (),
              false,                      // Configure-only.
              nullopt,                    // Checkout root.
              false,                      // Checkout purge.
              strings (),                 // Configuration variables.
              {config_package {mdb, ""}}, // Required by (command line).
              false,                      // Required by dependents.
              0};                         // State flags.

            build_package_refs dep_chain;

            pkgs.collect_build (o,
                                move (p),
                                find_prereq_database,
                                rpt_depts,
                                add_priv_cfg,
                                &postponed /* recursively */,
                                &dep_chain);
          }
        }

        // Handle the (combined) postponed collection.
        //
        if (!postponed.empty ())
          pkgs.collect_build_postponed (o,
                                        postponed,
                                        find_prereq_database,
                                        rpt_depts,
                                        add_priv_cfg);

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
        // deterministic. We, however, do them before hold_pkgs so that they
        // appear (e.g., on the plan) last.
        //
        for (const dep& d: deps)
          pkgs.order (d.db,
                      d.name,
                      nullopt               /* buildtime */,
                      find_prereq_database,
                      false                 /* reorder */);

        for (const build_package& p: reverse_iterate (hold_pkgs))
          pkgs.order (p.db,
                      p.name (),
                      nullopt /* buildtime */,
                      find_prereq_database);

        for (const auto& rd: rpt_depts)
          pkgs.order (rd.first.db,
                      rd.first.name,
                      nullopt               /* buildtime */,
                      find_prereq_database,
                      false                 /* reorder */);

        // Collect and order all the dependents that we will need to
        // reconfigure because of the up/down-grades of packages that are now
        // on the list.
        //
        pkgs.collect_order_dependents (rpt_depts);

        // And, finally, make sure all the packages that we need to unhold
        // are on the list.
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
                          nullopt               /* buildtime */,
                          find_prereq_database,
                          false                 /* reorder */);
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

        // Now, as we are done with package builds collecting/ordering, erase
        // the replacements from the repointed dependents prerequisite sets
        // and persist the changes.
        //
        for (auto& rd: rpt_depts)
        {
          database&           db (rd.first.db);
          const package_name& nm (rd.first.name);

          shared_ptr<selected_package> sp (db.load<selected_package> (nm));

          for (const auto& prq: rd.second)
          {
            if (prq.second) // Prerequisite replacement?
            {
              const config_package& cp (prq.first);

              size_t n (sp->prerequisites.erase (
                        lazy_shared_ptr<selected_package> (cp.db, cp.name)));

              // The selected package should always contain the prerequisite
              // replacement at this time, so its removal should always
              // succeed.
              //
              assert (n == 1);
            }
          }

          db.update (sp);
        }

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
                                  true /* simulate */,
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
        auto eval_dep = [&dep_pkgs, &rec_pkgs, &o] (
          database& db,
          const shared_ptr<selected_package>& sp,
          bool ignore_unsatisfiable = true) -> optional<evaluate_result>
        {
          optional<evaluate_result> r;

          // See if there is an optional dependency upgrade recommendation.
          //
          if (!sp->hold_package)
            r = evaluate_dependency (db,
                                     sp,
                                     dep_pkgs,
                                     o.no_move (),
                                     ignore_unsatisfiable);

          // If none, then see for the recursive dependency upgrade
          // recommendation.
          //
          // Let's skip upgrading system packages as they are, probably,
          // configured as such for a reason.
          //
          if (!r && !sp->system () && !rec_pkgs.empty ())
            r = evaluate_recursive (db, sp, rec_pkgs, ignore_unsatisfiable);

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

          // Here we scratch if evaluate changed its mind or if the resulting
          // version doesn't match what we expect it to be.
          //
          if (auto sp = db.find<selected_package> (i->name))
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
            scratch = true; // Rebuild the plan from scratch.
            i = deps.erase (i);
          }
          else
            ++i;
        }

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
          assert (!scratch); // No reason to change any previous decision.

          if (o.keep_unused () || o.no_refinement ())
            refine = false;
        }

        if (!scratch && refine)
        {
          // First, we check if the refinement is required, ignoring the
          // unsatisfiable dependency version constraints. If we end up
          // refining the execution plan, such dependencies might be dropped,
          // and then there will be nothing to complain about. When no more
          // refinements are necessary we will run the diagnostics check, to
          // make sure that the unsatisfiable dependency, if left, is
          // reported.
          //
          auto need_refinement = [&eval_dep, &deps, &rec_pkgs, &dep_dbs, &o] (
            bool diag = false) -> bool
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
                    deps.push_back (dep {er->db,
                                         sp->name,
                                         move (er->available),
                                         move (er->repository_fragment),
                                         er->system});

                  r = true;
                }
              }
            }

            return r;
          };

          refine = need_refinement ();

          if (!refine)
            need_refinement (true /* diag */);
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

          map<config_package, prerequisites> package_prereqs;
          small_vector<config_selected_package, 16> chain;

          auto verify_dependencies = [&package_prereqs, &chain]
                                     (database& db,
                                      shared_ptr<selected_package> sp,
                                      const auto& verify_dependencies)
               -> const prerequisites&
          {
            // Return the cached value, if present.
            //
            config_package cp {db, sp->name};
            {
              auto i (package_prereqs.find (cp));

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
            // dependency, since we only store its version constraint. The
            // current implementation relies on the fact that the build-time
            // dependency configuration type (host or build2) differs from the
            // dependent configuration type (target is a common case) and
            // doesn't work well, for example, for the self-hosted
            // configurations. For them it can fail erroneously. We can
            // potentially fix that by additionally storing the build-time
            // flag besides the version constraint. However, let's first see
            // if it ever becomes a problem.
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
            auto j (package_prereqs.emplace (move (cp), move (r)));
            assert (j.second); // A package cannot depend on itself.

            return j.first->second;
          };

          for (auto& p: build_pkgs)
            verify_dependencies (p.first,
                                 move (p.second),
                                 verify_dependencies);

          // Now, verify that none of the build2 modules may simultaneously be
          // built in multiple configurations, accross all (potentially
          // unrelated) dependency trees.
          //
          // For that we use the `package_prereqs` map: its key set refers to
          // all the packages potentially involved into the build (explicitly
          // or implicitly).
          //
          {
            map<package_name, database&> build2_mods;

            for (const auto& pp: package_prereqs)
            {
              const config_package& cp (pp.first);

              // Skip packages other than the build2 modules.
              //
              if (!build2_module (cp.name))
                continue;

              // Skip build2 modules configured as system.
              //
              {
                shared_ptr<selected_package> sp (
                  cp.db.find<selected_package> (cp.name));

                assert (sp != nullptr);

                if (sp->system ())
                  continue;
              }

              auto i (build2_mods.emplace (cp.name, cp.db));

              if (!i.second)
              {
                database& db (i.first->second);

                // The `package_prereqs` map can only contain the same package
                // twice if databases differ.
                //
                assert (db != cp.db);

                fail << "building build system module " << cp.name << " in "
                     << "multiple configurations" <<
                  info << db.config_orig <<
                  info << cp.db.config_orig;
              }
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
          // and the build package list.
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
              for (const auto& dps: ses.map ())
              {
                if (const selected_packages* sps = sp_session (dps.second))
                {
                  if (old_sp.find (dps.first) == old_sp.end ())
                    assert (sps->empty ());
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
      }
    }

    // Print what we are going to do, then ask for the user's confirmation.
    // While at it, detect if we have any dependents that the user may want to
    // update.
    //
    bool update_dependents (false);

    // We need the plan and to ask for the user's confirmation only if some
    // implicit action (such as building prerequisite or reconfiguring
    // dependent package) is to be taken or there is a selected package which
    // version must be changed. But if the user explicitly requested it with
    // --plan, then we print it as long as it is not empty.
    //
    string plan;
    sha256 csum;
    bool need_prompt (false);

    if (!o.yes ()           ||
        o.print_only ()     ||
        o.plan_specified () ||
        o.rebuild_checksum_specified ())
    {
      bool first (true); // First entry in the plan.

      for (const build_package& p: reverse_iterate (pkgs))
      {
        database& pdb (p.db);
        const shared_ptr<selected_package>& sp (p.selected);

        string act;

        assert (p.action);

        if (*p.action == build_package::drop)
        {
          act = "drop " + sp->string (pdb) + " (unused)";
          need_prompt = true;
        }
        else
        {
          string cause;
          if (*p.action == build_package::adjust)
          {
            assert (sp != nullptr && (p.reconfigure () || p.unhold ()));

            // This is a dependent needing reconfiguration.
            //
            // This is an implicit reconfiguration which requires the plan to
            // be printed. Will flag that later when composing the list of
            // prerequisites.
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
          }
          else
          {
            // Even if we already have this package selected, we have to
            // make sure it is configured and updated.
            //
            if (sp == nullptr)
              act = p.system ? "configure" : "new";
            else if (sp->version == p.available_version ())
            {
              // If this package is already configured and is not part of the
              // user selection (or we are only configuring), then there is
              // nothing we will be explicitly doing with it (it might still
              // get updated indirectly as part of the user selection update).
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
                      ? "reconfigure"
                      : "reconfigure/update")
                   : "update");
            }
            else
            {
              act = p.system
                ? "reconfigure"
                : sp->version < p.available_version ()
                  ? "upgrade"
                  : "downgrade";

              need_prompt = true;
            }

            if (p.unhold ())
              act += "/unhold";

            act += ' ' + p.available_name_version_db ();
            cause = p.required_by_dependents ? "required by" : "dependent of";

            if (p.configure_only ())
              update_dependents = true;
          }

          string rb;
          if (!p.user_selection ())
          {
            // Note: if we are ever tempted to truncate this, watch out for
            // the --rebuild-checksum functionality which uses this. But then
            // it's not clear this information is actually important: can a
            // dependent-dependency structure change without any of the
            // package versions changing? Doesn't feel like it should.
            //
            for (const config_package& cp: p.required_by)
              rb += (rb.empty () ? " " : ", ") + cp.string ();

            // If not user-selected, then there should be another (implicit)
            // reason for the action.
            //
            assert (!rb.empty ());

            need_prompt = true;
          }

          if (!rb.empty ())
            act += " (" + cause + rb + ')';
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
    // 1.  disfigure       up/down-graded, reconfigured [left to right]
    // 2.  purge           up/down-graded               [right to left]
    // 3.a fetch/unpack    new, up/down-graded
    // 3.b checkout        new, up/down-graded
    // 4.  configure       all
    // 5.  unhold          unheld
    // 6.  build           user selection               [right to left]
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
                              false /* simulate */,
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
    for (const build_package& p: reverse_iterate (pkgs))
    {
      assert (p.action);

      if (*p.action != build_package::build || p.configure_only ())
        continue;

      database& db (p.db);
      const shared_ptr<selected_package>& sp (p.selected);

      if (!sp->system () && // System package doesn't need update.
          p.user_selection ())
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

        if ((*p.action == build_package::adjust && p.reconfigure ()) ||
            (*p.action == build_package::build &&
             (p.flags & build_package::build_repoint) != 0))
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
                bool simulate,
                const function<find_database_function>& fdb)
  {
    tracer trace ("execute_plan");

    l4 ([&]{trace << "simulate: " << (simulate ? "yes" : "no");});

    bool r (false);
    uint16_t verb (!simulate ? bpkg::verb : 0);

    bool result (verb && !o.no_result ());
    bool progress (!result && verb == 1 && !o.no_progress () && stderr_term);

    size_t prog_i, prog_n, prog_percent;

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

    for (build_package& p: build_pkgs)
    {
      assert (p.action);

      if (!disfigure_pred (p))
        continue;

      database& pdb (p.db);
      shared_ptr<selected_package>& sp (p.selected);

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
        if (sp->external () && *p.action != build_package::drop)
        {
          const shared_ptr<available_package>& ap (p.available);

          // If adjustment or orphan, then new and old are the same.
          //
          if (ap == nullptr || ap->locations.empty ())
            external = true;
          else
          {
            const package_location& pl (ap->locations[0]);

            if (pl.repository_fragment.object_id () == "") // Special root.
              external = !exists (pl.location); // Directory case.
            else
            {
              // See if the package comes from the directory-based repository,
              // and so is external.
              //
              // Note that such repository fragments are always preferred over
              // others (see below).
              //
              for (const package_location& l: ap->locations)
              {
                if (l.repository_fragment.load ()->location.directory_based ())
                {
                  external = true;
                  break;
                }
              }
            }
          }
        }

        // Reset the keep_out flag if the package being unpacked is not
        // external.
        //
        if (p.keep_out && !external)
          p.keep_out = false;
      }

      // For an external package being replaced with another external, keep
      // the configuration unless requested not to with --disfigure.
      //
      // Note that for other cases the preservation of the configuration is
      // still a @@ TODO (the idea is to use our config.config.{save,load}
      // machinery).
      //
      // Commits the transaction.
      //
      pkg_disfigure (o, pdb, t,
                     sp,
                     !p.keep_out /* clean */,
                     p.disfigure || !external /* disfigure */,
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
        // up/down-grading.
        //
        if (sp == nullptr || sp->version != p.available_version ())
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
              const repository_location& rl (
                l.repository_fragment.load ()->location);

              if (!basis || rl.local ()) // First or local?
              {
                basis = rl.basis ();

                if (rl.directory_based ())
                  break;
              }
            }

            assert (basis);

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

    if (progress)
    {
      prog_i = 0;
      prog_n = static_cast<size_t> (count_if (build_pkgs.begin (),
                                              build_pkgs.end (),
                                              configure_pred));
      prog_percent = 100;
    }

    for (build_package& p: reverse_iterate (build_pkgs))
    {
      assert (p.action);

      if (!configure_pred (p))
        continue;

      shared_ptr<selected_package>& sp (p.selected);
      const shared_ptr<available_package>& ap (p.available);

      // Configure the package.
      //
      // At this stage the package is either selected, in which case it's a
      // source code one, or just available, in which case it is a system
      // one. Note that a system package gets selected as being configured.
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

      // Note that pkg_configure() commits the transaction.
      //
      if (p.system)
        sp = pkg_configure_system (ap->id.name,
                                   p.available_version (),
                                   pdb,
                                   t);
      else if (ap != nullptr)
        pkg_configure (o,
                       pdb,
                       t,
                       sp,
                       ap->dependencies,
                       p.config_vars,
                       simulate,
                       fdb);
      else // Dependent.
      {
        // Must be in the unpacked state since it was disfigured on the first
        // pass (see above).
        //
        assert (sp->state == package_state::unpacked);

        package_manifest m (
          pkg_verify (sp->effective_src_root (pdb.config_orig),
                      true /* ignore_unknown */,
                      [&sp] (version& v) {v = sp->version;}));

        pkg_configure (o,
                       p.db,
                       t,
                       sp,
                       convert (move (m.dependencies)),
                       p.config_vars,
                       simulate,
                       fdb);
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
