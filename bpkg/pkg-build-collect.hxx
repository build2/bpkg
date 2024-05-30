// file      : bpkg/pkg-build-collect.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_BUILD_COLLECT_HXX
#define BPKG_PKG_BUILD_COLLECT_HXX

#include <map>
#include <set>
#include <list>
#include <forward_list>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>
#include <bpkg/diagnostics.hxx>

#include <bpkg/common-options.hxx>
#include <bpkg/pkg-build-options.hxx>

#include <bpkg/database.hxx>
#include <bpkg/pkg-configure.hxx>          // find_database_function()
#include <bpkg/package-skeleton.hxx>
#include <bpkg/system-package-manager.hxx>

namespace bpkg
{
  // The current configurations dependents being "repointed" to prerequisites
  // in other configurations, together with their replacement flags. The flag
  // is true for the replacement prerequisites ("new") and false for the
  // prerequisites being replaced ("old"). The unamended prerequisites have no
  // entries.
  //
  using repointed_dependents =
    std::map<package_key, std::map<package_key, bool>>;

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
  // a different version. If that happens, we make sure that the replaced
  // package version doesn't apply constraints and/or configuration to its
  // own dependencies anymore and also that its non-shared dependencies are
  // gone from the map, recursively (see replaced_versions for details).
  // One notable side-effect of this process is that all the packages in the
  // map end up in the list.
  //
  // Note that we don't try to do exhaustive constraint satisfaction (i.e.,
  // there is no backtracking). Specifically, if we have two candidate
  // packages each satisfying a constraint of its dependent package, then if
  // neither of them satisfy both constraints, then we give up and ask the
  // user to resolve this manually by explicitly specifying the version that
  // will satisfy both constraints.
  //
  // Also note that we rule out dependency alternatives with enable constraint
  // that evaluates to false and try to select one satisfactory alternative if
  // there are multiple of them. In the latter case we pick the first
  // alternative with packages that are already used (as a result of being
  // dependencies of other package, requested by the user, or already being
  // present in the configuration) and fail if such an alternative doesn't
  // exist.
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

    // If we end up collecting the prerequisite builds for this package, then
    // this member stores copies of the selected dependency alternatives. The
    // dependency alternatives for toolchain build-time dependencies and for
    // dependencies which have all the alternatives disabled are represented
    // as empty dependency alternatives lists. If present, it is parallel to
    // the available package's dependencies member.
    //
    // Initially nullopt. Can be filled partially if the package prerequisite
    // builds collection is postponed for any reason (see postponed_packages
    // and postponed_configurations for possible reasons).
    //
    optional<bpkg::dependencies> dependencies;

    // Indexes of the selected dependency alternatives stored in the above
    // dependencies member.
    //
    optional<vector<size_t>> alternatives;

    // If we end up collecting the prerequisite builds for this package, then
    // this member stores the skeleton of the package being built.
    //
    // Initially nullopt. Can potentially be loaded but with the reflection
    // configuration variables collected only partially if the package
    // prerequisite builds collection is postponed for any reason. Can also be
    // unloaded if the package has no conditional dependencies.
    //
    optional<package_skeleton> skeleton;

    // If the package prerequisite builds collection is postponed, then this
    // member stores the references to the enabled alternatives (in available
    // package) of a dependency being the cause of the postponement together
    // with their original indexes in the respective dependency alternatives
    // list. This, in particular, allows us not to re-evaluate conditions
    // multiple times on the re-collection attempts.
    //
    // Note: it shouldn't be very common for a dependency to contain more than
    // two true alternatives.
    //
    using dependency_alternatives_refs =
      small_vector<pair<reference_wrapper<const dependency_alternative>,
                        size_t>,
                   2>;

    optional<dependency_alternatives_refs> postponed_dependency_alternatives;

    // True if the recursive collection of the package has been started or
    // performed.
    //
    // Used by the dependency configuration negotiation machinery which makes
    // sure that its configuration is negotiated between dependents before its
    // recursive collection is started (see postponed_configurations for
    // details).
    //
    // Note that the dependencies member cannot be used for that purpose since
    // it is not always created (think of a system dependency or an existing
    // dependency that doesn't need its prerequisites re-collection). In a
    // sense the recursive collection flag is a barrier for the dependency
    // configuration negotiation.
    //
    bool recursive_collection;

    // Return true if the recursive collection started but has been postponed
    // for any reason.
    //
    bool
    recursive_collection_postponed () const;

    // Hold flags.
    //
    // Note that we only "increase" the hold_package value that is already in
    // the selected package, unless the adjust_unhold flag is set (see below).
    //
    optional<bool> hold_package;

    // Note that it is perfectly valid for the hold_version flag to be false
    // while the command line constraint is present in the constraints list
    // (see below). This may happen if the package build is collected by the
    // unsatisfied dependency constraints resolution logic (see
    // try_replace_dependency() in pkg-build.cxx for details).
    //
    optional<bool> hold_version;

    // Constraint value plus, normally, the dependent package name/version
    // that placed this constraint but can also be some other name (in which
    // case the version is absent) for the initial selection. Currently, the
    // only valid non-package name is 'command line', which is used when the
    // package version is constrained by the user on the command line.
    //
    // Note that if the dependent is a package name, then this package is
    // expected to be collected (present in the map).
    //
    struct constraint_type
    {
      version_constraint value;

      package_version_key dependent;

      // False for non-packages. Otherwise, indicates whether the constraint
      // comes from the existing rather than the being built dependent.
      //
      bool existing_dependent;

      // Create constraint for a package dependent.
      //
      constraint_type (version_constraint v,
                       database& db,
                       package_name nm,
                       version ver,
                       bool e)
          : value (move (v)),
            dependent (db, move (nm), move (ver)),
            existing_dependent (e) {}

      // Create constraint for a non-package dependent.
      //
      constraint_type (version_constraint v, database& db, string nm)
          : value (move (v)),
            dependent (db, move (nm)),
            existing_dependent (false) {}
    };

    vector<constraint_type> constraints;

    // System package indicator. See also a note in the merge() function.
    //
    bool system;

    // Return the system/distribution package status if this is a system
    // package (re-)configuration and the package is being managed by the
    // system package manager (as opposed to user/fallback). Otherwise, return
    // NULL (so can be used as bool).
    //
    // Note on terminology: We call the bpkg package that is being configured
    // as available from the system as "system package" and we call the
    // underlying package managed by the system/distribution package manager
    // as "system/distribution package". See system-package-manager.hxx for
    // background.
    //
    const system_package_status*
    system_status () const;

    // As above but only return the status if the package needs to be
    // installed.
    //
    const system_package_status*
    system_install () const;

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

    // If present, then the package is requested to be upgraded (true) or
    // patched (false). Can only be present if the package is already
    // selected. Can only be false if the selected package version is
    // patchable. Used by the unsatisfied dependency constraints resolution
    // logic (see try_replace_dependency() in pkg-build.cxx for details).
    //
    optional<bool> upgrade;

    // If true, then this package is requested to be deorphaned. Can only be
    // true if the package is already selected and is orphaned. Used by the
    // unsatisfied dependency constraints resolution logic (see
    // try_replace_dependency() in pkg-build.cxx for details).
    //
    bool deorphan;

    // Set of packages (dependents or dependencies but not a mix) that caused
    // this package to be built or adjusted. The 'command line' name signifies
    // user selection and can be present regardless of the
    // required_by_dependents flag value.
    //
    // Note that if this is a package name, then this package is expected to
    // be collected (present in the map), potentially just pre-entered if
    // required_by_dependents is false. If required_by_dependents is true,
    // then the packages in the set are all expected to be collected as builds
    // (action is build, available is not NULL, etc).
    //
    // Also note that if required_by_dependents is true, then all the
    // dependent package versions in the required_by set are expected to be
    // known (the version members are not empty). Otherwise (the required_by
    // set contains dependencies), since it's not always easy to deduce the
    // dependency versions at the time of collecting the dependent build (see
    // collect_repointed_dependents() implementation for details), the
    // dependency package versions are expected to all be unknown.
    //
    std::set<package_version_key> required_by;

    // If this flag is true, then required_by contains dependents.
    //
    // We need this because required_by packages have different semantics for
    // different actions: the dependent for regular builds and dependency for
    // adjustments and repointed dependent reconfiguration builds. Mixing them
    // would break prompts/diagnostics.
    //
    bool required_by_dependents;

    // Consider a package as user-selected if it is specified on the command
    // line, is a held package being upgraded via the `pkg-build -u|-p`
    // command form, or is a dependency being upgraded via the recursively
    // upgraded dependent.
    //
    bool
    user_selection () const;

    // Consider a package as user-selected only if it is specified on the
    // command line as build to hold.
    //
    bool
    user_selection (const vector<build_package>& hold_pkgs) const;

    // Return true if the configured package needs to be recollected
    // recursively.
    //
    // This is required if it is being built as a source package and needs to
    // be up/down-graded and/or reconfigured and has some buildfile clauses,
    // it is a repointed dependent, or it is already in the process of being
    // collected. Also configured dependents can be scheduled for recollection
    // explicitly (see postponed_packages and build_recollect flag for
    // details).
    //
    bool
    recollect_recursively (const repointed_dependents&) const;

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
    reconfigure () const;

    // Set if this build action is for repointing of prerequisite.
    //
    static const uint16_t build_repoint = 0x0004;

    // Set if this build action is for re-evaluating of an existing dependent.
    //
    static const uint16_t build_reevaluate = 0x0008;

    // Set if this build action is for recursive re-collecting of an existing
    // dependent due to deviation, detecting merge configuration cycle, etc.
    //
    static const uint16_t build_recollect = 0x0010;

    // Set if this build action is for replacing of an existing package due to
    // deorphaning or rebuilding as an archive or directory.
    //
    // Note that to replace a package we need to re-fetch it from an existing
    // repository fragment, archive, or directory (even if its version doesn't
    // change).
    //
    static const uint16_t build_replace = 0x0020;

    bool
    replace () const
    {
      return (flags & build_replace) != 0;
    }

    bool
    configure_only () const;

    // Return true if the resulting package will be configured as external.
    // Optionally, if the package is external, return its absolute and
    // normalized source root directory path.
    //
    bool
    external (dir_path* = nullptr) const;

    // If the resulting package will be configured as external, then return
    // its absolute and normalized source root directory path and nullopt
    // otherwise.
    //
    optional<dir_path>
    external_dir () const
    {
      dir_path r;
      return external (&r) ? optional<dir_path> (move (r)) : nullopt;
    }

    const version&
    available_version () const;

    string
    available_name_version () const
    {
      assert (available != nullptr);
      return package_string (available->id.name, available_version (), system);
    }

    string
    available_name_version_db () const;

    // Merge constraints, required-by package names, hold_* flags, state
    // flags, and user-specified options/variables.
    //
    void
    merge (build_package&&);

    // Initialize the skeleton of a being built package.
    //
    package_skeleton&
    init_skeleton (const common_options&,
                   bool load_old_dependent_config = true,
                   const shared_ptr<available_package>& override = nullptr);
  };

  using build_package_list = std::list<reference_wrapper<build_package>>;

  using build_package_refs =
    small_vector<reference_wrapper<const build_package>, 16>;

  // Packages with postponed prerequisites collection, for one of the
  // following reasons:
  //
  // - Postponed due to the inability to find a dependency version satisfying
  //   the pre-entered constraint from repositories available to this
  //   package. The idea is that this constraint could still be satisfied from
  //   a repository fragment of some other package (that we haven't processed
  //   yet) that also depends on this prerequisite.
  //
  // - Postponed due to the inability to choose between two dependency
  //   alternatives, both having dependency packages which are not yet
  //   selected in the configuration nor being built. The idea is that this
  //   ambiguity could still be resolved after some of those dependency
  //   packages get built via some other dependents.
  //
  // - Postponed recollection of configured dependents whose dependencies
  //   up/downgrade causes selection of different dependency alternatives.
  //   This, in particular, may end up in resolving different dependency
  //   packages and affect the dependent and dependency configurations.
  //
  // - Postponed recollection of configured dependents for resolving merge
  //   configuration cycles and as a fallback for missed re-evaluations due to
  //   the shadow-based configuration clusters merge (see
  //   collect_build_prerequisites() for details).
  //
  // For the sake of testing, make sure the order in the set is stable.
  //
  struct compare_build_package
  {
    bool
    operator() (const build_package* x, const build_package* y) const
    {
      const package_name& nx (x->name ());
      const package_name& ny (y->name ());

      if (int d = nx.compare (ny))
        return d < 0;

      return x->db.get () < y->db.get ();
    }
  };
  using postponed_packages = std::set<build_package*, compare_build_package>;

  // Base for exception types that indicate an inability to collect a package
  // build because it was collected prematurely (version needs to be replaced,
  // configuration requires further negotiation, etc).
  //
  struct scratch_collection
  {
    // Only used for tracing.
    //
    const char* description;
    const package_key* package = nullptr; // Could be NULL.

    explicit
    scratch_collection (const char* d): description (d) {}
  };

  // Map of dependency packages whose recursive processing should be postponed
  // because they have dependents with configuration clauses.
  //
  // Note that dependents of such a package that don't have any configuration
  // clauses are processed right away (since the negotiated configuration may
  // not affect them) while those that do are postponed in the same way as
  // those with dependency alternatives (see above).
  //
  // Note that the latter kind of dependent is what eventually causes
  // recursive processing of the dependency packages. Which means we must
  // watch out for bogus entries in this map which we may still end up with
  // (e.g., because postponement caused cross-talk between dependency
  // alternatives). Thus we keep flags that indicate whether we have seen each
  // type of dependent and then just process dependencies that have the first
  // (without config) but not the second (with config).
  //
  // Note that if any of these flags is set to true, then the dependency is
  // expected to be collected (present in the build_packages's map; see below
  // for the class definition).
  //
  struct postponed_dependency
  {
    bool wout_config; // Has dependent without config.
    bool with_config; // Has dependent with config.

    postponed_dependency (bool woc, bool wic)
        : wout_config (woc),
          with_config (wic) {}

    bool
    bogus () const {return wout_config && !with_config;}
  };

  class postponed_dependencies: public std::map<package_key,
                                                postponed_dependency>
  {
  public:
    bool
    has_bogus () const
    {
      for (const auto& pd: *this)
      {
        if (pd.second.bogus ())
          return true;
      }
      return false;
    }

    // Erase the bogus postponements and throw cancel_postponement, if any.
    //
    struct cancel_postponement: scratch_collection
    {
      cancel_postponement ()
          : scratch_collection (
            "bogus dependency collection postponement cancellation") {}
    };

    void
    cancel_bogus (tracer& trace)
    {
      bool bogus (false);
      for (auto i (begin ()); i != end (); )
      {
        const postponed_dependency& d (i->second);

        if (d.bogus ())
        {
          bogus = true;

          l5 ([&]{trace << "erase bogus postponement " << i->first;});

          i = erase (i);
        }
        else
          ++i;
      }

      if (bogus)
      {
        l5 ([&]{trace << "bogus postponements erased, throwing";});
        throw cancel_postponement ();
      }
    }
  };

  // Map of the dependencies whose recursive collection is postponed until
  // their existing dependents re-collection/re-evaluation to the lists of the
  // respective existing dependents (see collect_build_prerequisites() for
  // details).
  //
  using postponed_existing_dependencies = std::map<package_key,
                                                   vector<package_key>>;

  // Set of dependency alternatives which were found unacceptable by the
  // configuration negotiation machinery and need to be ignored on re-
  // collection.
  //
  // Note that while negotiating the dependency alternative configuration for
  // a dependent it may turn out that the configuration required by other
  // dependents is not acceptable for this dependent. It can also happen that
  // this dependent is involved in a negotiation cycle when two dependents
  // continuously overwrite each other's configuration during re-negotiation.
  // Both situations end up with the failure, unless the dependent has some
  // other reused dependency alternative which can be tried instead. In the
  // latter case, we note the problematic alternative and re-collect from
  // scratch. On re-collection the unacceptable alternatives are ignored,
  // similar to the disabled alternatives.
  //
  struct unacceptable_alternative
  {
    package_key package;
    bpkg::version version;
    pair<size_t, size_t> position; // depends + alternative (1-based)

    unacceptable_alternative (package_key pkg,
                              bpkg::version ver,
                              pair<size_t, size_t> pos)
        : package (move (pkg)), version (move (ver)), position (pos) {}

    bool
    operator< (const unacceptable_alternative& v) const
    {
      if (package != v.package)
        return package < v.package;

      if (int r = version.compare (v.version))
        return r < 0;

      return position < v.position;
    }
  };

  using unacceptable_alternatives = std::set<unacceptable_alternative>;

  struct unaccept_alternative: scratch_collection
  {
    unaccept_alternative (): scratch_collection ("unacceptable alternative") {}
  };

  // Map of packages which need to be re-collected with the different version
  // and/or system flag or dropped.
  //
  // Note that the initial package version may be adjusted to satisfy
  // constraints of dependents discovered during the packages collection. It
  // may also be dropped if this is a dependency which turns out to be unused.
  // However, it may not always be possible to perform such an adjustment
  // in-place since the intermediate package version could already apply some
  // constraints and/or configuration to its own dependencies. Thus, we may
  // need to note the desired package version information and re-collect from
  // scratch.
  //
  // Also note that during re-collection such a desired version may turn out
  // to not be a final version and the adjustment/re-collection can repeat.
  //
  // And yet, it doesn't seem plausible to ever create a replacement for the
  // drop: replacing one drop with another is meaningless (all drops are the
  // same) and replacing the package drop with a package version build can
  // always been handled in-place.
  //
  // On the first glance, the map entries which have not been used for
  // replacement during the package collection (bogus entries) are harmless
  // and can be ignored. However, the dependency configuration negotiation
  // machinery refers to this map and skips existing dependents with
  // configuration clause which belong to it (see query_existing_dependents()
  // for details). Thus, if after collection of packages some bogus entries
  // are present in the map, then it means that we could have erroneously
  // skipped some existing dependents because of them and so need to erase
  // these entries and re-collect.
  //
  struct replaced_version
  {
    // Desired package version, repository fragment, and system flag.
    //
    // Both are NULL for the replacement with the drop.
    //
    shared_ptr<available_package> available;
    lazy_shared_ptr<bpkg::repository_fragment> repository_fragment;
    bool system; // Meaningless for the drop.

    // True if the entry has been inserted or used for the replacement during
    // the current (re-)collection iteration. Used to keep track of "bogus"
    // (no longer relevant) entries.
    //
    bool replaced;

    // Create replacement with the different version.
    //
    replaced_version (shared_ptr<available_package> a,
                      lazy_shared_ptr<bpkg::repository_fragment> f,
                      bool s)
        : available (move (a)),
          repository_fragment (move (f)),
          system (s),
          replaced (true) {}

    // Create replacement with the drop.
    //
    replaced_version (): system (false), replaced (true) {}
  };

  class replaced_versions: public std::map<package_key, replaced_version>
  {
  public:
    // Erase the bogus replacements and, if any, throw cancel_replacement, if
    // requested.
    //
    struct cancel_replacement: scratch_collection
    {
      cancel_replacement ()
          : scratch_collection ("bogus version replacement cancellation") {}
    };

    void
    cancel_bogus (tracer&, bool scratch);
  };


  // Dependents with their unsatisfactory dependencies and the respective
  // ignored constraints.
  //
  // Note that during the collecting of all the explicitly specified packages
  // and their dependencies for the build, we may discover that a being
  // up/downgraded dependency doesn't satisfy all the being reconfigured,
  // up/downgraded, or newly built dependents. Rather than fail immediately in
  // such a case, we postpone the failure, add the unsatisfied dependents and
  // their respective constraints to the unsatisfied dependents list, and
  // continue the collection/ordering in the hope that these problems will be
  // resolved naturally as a result of the requested recollection from scratch
  // or execution plan refinement (dependents will also be up/downgraded or
  // dropped, dependencies will be up/downgraded to a different versions,
  // etc).
  //
  // Also note that after collecting/ordering of all the explicitly specified
  // packages and their dependencies for the build we also collect/order their
  // existing dependents for reconfiguration, recursively. It may happen that
  // some of the up/downgraded dependencies don't satisfy the version
  // constraints which some of the existing dependents impose on them. Rather
  // than fail immediately in such a case, we postpone the failure, add this
  // dependent and the unsatisfactory dependency to the unsatisfied dependents
  // list, and continue the collection/ordering in the hope that these
  // problems will be resolved naturally as a result of the execution plan
  // refinement.
  //
  // And yet, if these problems do not resolve naturally, then we still try to
  // resolve them by finding dependency versions which satisfy all the imposed
  // constraints.
  //
  // Specifically, we cache such unsatisfied dependents/constraints, pretend
  // that the dependents don't impose them and proceed with the remaining
  // collecting/ordering, simulating the plan execution, and evaluating the
  // dependency versions. After that, if scratch_collection exception has not
  // been thrown, we check if the execution plan is finalized or a further
  // refinement is required. In the latter case we drop the cache and proceed
  // with the next iteration of the execution plan refinement which may
  // resolve these problems naturally. Otherwise, we pick the first collected
  // unsatisfactory dependency and try to find the best available version,
  // considering all the constraints imposed by the user (explicit version
  // constraint, --patch and/or --deorphan options, etc) as well as by its new
  // and existing dependents. If the search succeeds, we update an existing
  // package spec or add the new one to the command line and recollect from
  // the very beginning. Note that we always add a new spec with the
  // hold_version flag set to false. If the search fails, then, similarily, we
  // try to find the replacement for some of the dependency's dependents,
  // recursively. Note that we track the package build replacements and never
  // repeat a replacement for the same command line state (which we adjust for
  // each replacement). If no replacement is deduced, then we roll back the
  // latest command line adjustment and recollect from the very beginning. If
  // there are no adjustments left to try, then we give up the resolution
  // search and report the first encountered unsatisfied (and ignored)
  // dependency constraint and fail.
  //
  // Note that while we are trying to pick a dependent replacement for the
  // subsequent re-collection, we cannot easily detect if the replacement is
  // satisfied with the currently collected dependencies since that would
  // effectively require to collect the replacement (select dependency
  // alternatives, potentially re-negotiate dependency configurations,
  // etc). Thus, we only verify that the replacement version satisfies its
  // currently collected dependents. To reduce the number of potential
  // dependent replacements to consider, we apply the heuristics and only
  // consider those dependents which have or may have some satisfaction
  // problems (not satisfied with a collected dependency, apply a dependency
  // constraint which is incompatible with other dependents, etc; see
  // try_replace_dependent() for details).
  //
  struct unsatisfied_constraint
  {
    // Note: also contains the unsatisfied dependent information.
    //
    build_package::constraint_type constraint;

    // Available package version which satisfies the above constraint.
    //
    version available_version;
    bool    available_system;
  };

  struct ignored_constraint
  {
    package_key dependency;
    version_constraint constraint;

    // Only specified when the failure is postponed during the collection of
    // the explicitly specified packages and their dependencies.
    //
    vector<unsatisfied_constraint> unsatisfied_constraints;
    vector<package_key> dependency_chain;

    ignored_constraint (const package_key& d,
                        const version_constraint& c,
                        vector<unsatisfied_constraint>&& ucs = {},
                        vector<package_key>&& dc = {})
        : dependency (d),
          constraint (c),
          unsatisfied_constraints (move (ucs)),
          dependency_chain (move (dc)) {}
  };

  struct unsatisfied_dependent
  {
    package_key dependent;
    vector<ignored_constraint> ignored_constraints;
  };

  struct build_packages;

  class unsatisfied_dependents: public vector<unsatisfied_dependent>
  {
  public:
    // Add a dependent together with the ignored dependency constraint and,
    // potentially, with the unsatisfied constraints and the dependency chain.
    //
    void
    add (const package_key& dependent,
         const package_key& dependency,
         const version_constraint&,
         vector<unsatisfied_constraint>&& ucs = {},
         vector<package_key>&& dc = {});

    // Try to find the dependent entry and return NULL if not found.
    //
    unsatisfied_dependent*
    find_dependent (const package_key&);

    // Issue the diagnostics for the first unsatisfied (and ignored)
    // dependency constraint and throw failed.
    //
    [[noreturn]] void
    diag (const build_packages&);
  };

  // List of dependency groups whose recursive processing should be postponed
  // due to dependents with configuration clauses, together with these
  // dependents (we will call them package clusters).
  //
  // The idea is that configuration for the dependencies in the cluster needs
  // to be negotiated between the dependents in the cluster. Note that at any
  // given time during collection a dependency can only belong to a single
  // cluster. For example, the following dependent/dependencies with
  // configuration clauses:
  //
  // foo: depends: libfoo
  // bar: depends: libfoo
  //      depends: libbar
  // baz: depends: libbaz
  //
  // End up in the following clusters (see string() below for the cluster
  // representation):
  //
  // {foo bar | libfoo->{foo/1,1 bar/1,1}}
  // {bar     | libbar->{bar/2,1}}
  // {baz     | libbaz->{baz/1,1}}
  //
  // Or, another example:
  //
  // foo: depends: libfoo
  // bar: depends: libfoo libbar
  // baz: depends: libbaz
  //
  // {foo bar | libfoo->{foo/1,1 bar/1,1} libbar->{bar/1,1}}
  // {baz     | libbaz->{baz/1,1}}
  //
  // Note that a dependent can belong to any given non-negotiated cluster with
  // only one `depends` position. However, if some dependency configuration is
  // up-negotiated for a dependent, then multiple `depends` positions will
  // correspond to this dependent in the same cluster. Naturally, such
  // clusters are always (being) negotiated.
  //
  // Note that adding new dependent/dependencies to the postponed
  // configurations can result in merging some of the existing clusters if the
  // dependencies being added intersect with multiple clusters. For example,
  // adding:
  //
  // fox: depends: libbar libbaz
  //
  // to the clusters in the second example will merge them into a single
  // cluster:
  //
  // {foo bar baz fox | libfoo->{foo/1,1 bar/1,1} libbar->{bar/1,1 fox/1,1}
  //                    libbaz->{baz/1,1 fox/1,1}}
  //
  // Also note that we keep track of packages which turn out to be
  // dependencies of existing (configured) dependents with configuration
  // clauses. The recursive processing of such packages should be postponed
  // until negotiation between all the existing and new dependents which may
  // or may not be present.
  //
  class postponed_configuration
  {
  public:
    // The id of the cluster plus the ids of all the clusters that have been
    // merged into it directly or as their components.
    //
    size_t id;
    small_vector<size_t, 1> merged_ids;

    using packages = small_vector<package_key, 1>;

    class dependency: public packages
    {
    public:
      pair<size_t, size_t> position; // depends + alternative (1-based)

      // If true, then another dependency alternative is present and that can
      // potentially be considered instead of this one (see
      // unacceptable_alternatives for details).
      //
      // Initially nullopt for existing dependents until they are re-evaluated.
      //
      optional<bool> has_alternative;

      dependency (const pair<size_t, size_t>& pos,
                  packages deps,
                  optional<bool> ha)
          : packages (move (deps)), position (pos), has_alternative (ha) {}
    };

    class dependent_info
    {
    public:
      bool existing;
      small_vector<dependency, 1> dependencies;

      dependency*
      find_dependency (pair<size_t, size_t> pos);

      void
      add (dependency&&);
    };

    using dependents_map = std::map<package_key, dependent_info>;

    dependents_map dependents;
    packages       dependencies;

    // Dependency configuration.
    //
    // Note that this container may not yet contain some entries that are
    // already in the dependencies member above. And it may already contain
    // entries that are not yet in dependencies due to the retry_configuration
    // logic.
    //
    package_configurations dependency_configurations;

    // Shadow clusters.
    //
    // See the collect lambda in collect_build_prerequisites() for details.
    //
    using positions = small_vector<pair<size_t, size_t>, 1>;
    using shadow_dependents_map = std::map<package_key, positions>;

    shadow_dependents_map shadow_cluster;

    // Absent -- not negotiated yet, false -- being negotiated, true -- has
    // been negotiated.
    //
    optional<bool> negotiated;

    // The depth of the negotiating recursion (see collect_build_postponed()
    // for details).
    //
    // Note that non-zero depth for an absent negotiated member indicates that
    // the cluster is in the existing dependents re-evaluation or
    // configuration refinment phases.
    //
    size_t depth = 0;

    // Add dependencies of a new dependent.
    //
    postponed_configuration (size_t i,
                             package_key&& dependent,
                             bool existing,
                             pair<size_t, size_t> position,
                             packages&& deps,
                             optional<bool> has_alternative)
        : id (i)
    {
      add (move (dependent),
           existing,
           position,
           move (deps),
           has_alternative);
    }

    // Add dependency of an existing dependent.
    //
    postponed_configuration (size_t i,
                             package_key&& dependent,
                             pair<size_t, size_t> position,
                             package_key&& dep)
        : id (i)
    {
      add (move (dependent),
           true /* existing */,
           position,
           packages ({move (dep)}),
           nullopt /* has_alternative */);
    }

    // Add dependencies of a dependent.
    //
    // Note: adds the specified dependencies to the end of the configuration
    // dependencies list suppressing duplicates.
    //
    void
    add (package_key&& dependent,
         bool existing,
         pair<size_t, size_t> position,
         packages&& deps,
         optional<bool> has_alternative);

    // Return true if any of the configuration's dependents depend on the
    // specified package.
    //
    bool
    contains_dependency (const package_key& d) const
    {
      return find (dependencies.begin (), dependencies.end (), d) !=
             dependencies.end ();
    }

    // Return true if this configuration contains any of the specified
    // dependencies.
    //
    bool
    contains_dependency (const packages&) const;

    // Return true if this and specified configurations contain any common
    // dependencies.
    //
    bool
    contains_dependency (const postponed_configuration&) const;

    // Notes:
    //
    // - Adds dependencies of the being merged from configuration to the end
    //   of the current configuration dependencies list suppressing
    //   duplicates.
    //
    // - Doesn't change the negotiate member of this configuration.
    //
    void
    merge (postponed_configuration&&);

    void
    set_shadow_cluster (postponed_configuration&&);

    bool
    is_shadow_cluster (const postponed_configuration&);

    bool
    contains_in_shadow_cluster (package_key dependent,
                                pair<size_t, size_t> pos) const;

    // Return the postponed configuration string representation in the form:
    //
    // {<dependent>[ <dependent>]* | <dependency>[ <dependency>]*}['!'|'?']
    //
    // <dependent>  = <package>['^']
    // <dependency> = <package>->{<dependent>/<position>[ <dependent>/<position>]*}
    //
    // The potential trailing '!' or '?' of the configuration representation
    // indicates that the configuration is negotiated or is being negotiated,
    // respectively.
    //
    // '^' character that may follow a dependent indicates that this is an
    // existing dependent.
    //
    // <position> = <depends-index>','<alternative-index>
    //
    // <depends-index> and <alternative-index> are the 1-based serial numbers
    // of the respective depends value and the dependency alternative in the
    // dependent's manifest.
    //
    // See package_key for details on <package>.
    //
    // For example:
    //
    // {foo^ bar | libfoo->{foo/2,3 bar/1,1} libbar->{bar/1,1}}!
    //
    std::string
    string () const;

  private:
    // Add the specified packages to the end of the dependencies list
    // suppressing duplicates.
    //
    void
    add_dependencies (packages&&);

    void
    add_dependencies (const packages&);
  };

  inline ostream&
  operator<< (ostream& os, const postponed_configuration& c)
  {
    return os << c.string ();
  }

  // Note that we could be adding new/merging existing entries while
  // processing an entry. Thus we use a list.
  //
  class postponed_configurations:
    public std::forward_list<postponed_configuration>
  {
  public:
    // Return the configuration the dependent is added to (after all the
    // potential configuration merges, etc).
    //
    // Also return in second absent if the merge happened due to the shadow
    // cluster logic (in which case the cluster was/is being negotiated),
    // false if any non-negotiated or being negotiated clusters has been
    // merged in, and true otherwise.
    //
    // If some configurations needs to be merged and this involves the (being)
    // negotiated configurations, then merge into the outermost-depth
    // negotiated configuration (with minimum non-zero depth).
    //
    pair<postponed_configuration&, optional<bool>>
    add (package_key dependent,
         bool existing,
         pair<size_t, size_t> position,
         postponed_configuration::packages dependencies,
         optional<bool> has_alternative);

    // Add new postponed configuration cluster with a single dependency of an
    // existing dependent.
    //
    // Note that it's the caller's responsibility to make sure that the
    // dependency doesn't already belong to any existing cluster.
    //
    void
    add (package_key dependent,
         pair<size_t, size_t> position,
         package_key dependency);

    postponed_configuration*
    find (size_t id);

    // Return address of the cluster the dependency belongs to and NULL if it
    // doesn't belong to any cluster.
    //
    const postponed_configuration*
    find_dependency (const package_key& d) const;

    // Return true if all the configurations have been negotiated.
    //
    bool
    negotiated () const;

    // Translate index to iterator and return the referenced configuration.
    //
    postponed_configuration&
    operator[] (size_t);

    size_t
    size () const;

  private:
    size_t next_id_ = 1;
  };

  struct build_packages: build_package_list
  {
    build_packages () = default;

    // Copy-constructible and move-assignable (used for snapshoting).
    //
    build_packages (const build_packages&);

    build_packages (build_packages&&) = delete;

    build_packages& operator= (const build_packages&) = delete;

    build_packages&
    operator= (build_packages&&) noexcept (false);

    // Pre-enter a build_package without an action. No entry for this package
    // may already exists.
    //
    void
    enter (package_name, build_package);

    // Return the package pointer if it is already in the map and NULL
    // otherwise (so can be used as bool).
    //
    build_package*
    entered_build (database& db, const package_name& name)
    {
      auto i (map_.find (db, name));
      return i != map_.end () ? &i->second.package : nullptr;
    }

    build_package*
    entered_build (const package_key& p)
    {
      return entered_build (p.db, p.name);
    }

    const build_package*
    entered_build (database& db, const package_name& name) const
    {
      auto i (map_.find (db, name));
      return i != map_.end () ? &i->second.package : nullptr;
    }

    const build_package*
    entered_build (const package_key& p) const
    {
      return entered_build (p.db, p.name);
    }

    // Return NULL if the dependent in the constraint is not a package name
    // (command line, etc; see build_package::constraint_type for details).
    // Otherwise, return the dependent package build which is expected to be
    // collected.
    //
    const build_package*
    dependent_build (const build_package::constraint_type&) const;

    // Collect the package being built. Return its pointer if this package
    // version was, in fact, added to the map and NULL if it was already there
    // and the existing version was preferred or if the package build has been
    // replaced with the drop. So can be used as bool.
    //
    // Consult replaced_vers for an existing version replacement entry and
    // follow it, if present, potentially collecting the package drop instead.
    // Ignore the entry if its version doesn't satisfy the specified
    // dependency constraints or the entry is a package drop and the specified
    // required-by package names have the "required by dependents" semantics.
    // In this case it's likely that this replacement will be applied for some
    // later collect_build() call but can potentially turn out bogus. Note
    // that a version replacement for a specific package may only be applied
    // once during the collection iteration.
    //
    // Add entry to replaced_vers and throw replace_version if the
    // existing version needs to be replaced but the new version cannot be
    // re-collected recursively in-place (see replaced_versions for details).
    //
    // Optionally, pass the function which verifies the chosen package
    // version. It is called before replace_version is potentially thrown or
    // the recursive collection is performed. The scratch argument is true if
    // the package version needs to be replaced but in-place replacement is
    // not possible (see replaced_versions for details).
    //
    // Also, in the recursive mode (find database function is not NULL):
    //
    // - Use the custom search function to find the package dependency
    //   databases.
    //
    // - For the repointed dependents collect the prerequisite replacements
    //   rather than prerequisites being replaced.
    //
    // - Call add_priv_cfg_function callback for the created private
    //   configurations.
    //
    // Note that postponed_* arguments must all be either specified or not.
    // The dep_chain argument can be specified in the non-recursive mode (for
    // the sake of the diagnostics) and must be specified in the recursive
    // mode.
    //
    struct replace_version: scratch_collection
    {
      replace_version (): scratch_collection ("package version replacement") {}
    };

    using add_priv_cfg_function = void (database&, dir_path&&);

    using verify_package_build_function = void (const build_package&,
                                                bool scratch);

    build_package*
    collect_build (const pkg_build_options&,
                   build_package,
                   replaced_versions&,
                   postponed_configurations&,
                   unsatisfied_dependents&,
                   build_package_refs* dep_chain = nullptr,
                   const function<find_database_function>& = nullptr,
                   const function<add_priv_cfg_function>& = nullptr,
                   const repointed_dependents* = nullptr,
                   postponed_packages* postponed_repo = nullptr,
                   postponed_packages* postponed_alts = nullptr,
                   postponed_packages* postponed_recs = nullptr,
                   postponed_existing_dependencies* = nullptr,
                   postponed_dependencies* = nullptr,
                   unacceptable_alternatives* = nullptr,
                   const function<verify_package_build_function>& = nullptr);

    // Collect prerequisites of the package being built recursively. Return
    // nullopt, unless in the pre-reevaluation mode (see below).
    //
    // But first "prune" this process if the package we build is a system one
    // or is already configured, since that would mean all its prerequisites
    // are configured as well. Note that this is not merely an optimization:
    // the package could be an orphan in which case the below logic will fail
    // (no repository fragment in which to search for prerequisites). By
    // skipping the prerequisite check we are able to gracefully handle
    // configured orphans.
    //
    // There are, however, some cases when we still need to re-collect
    // prerequisites of a configured package:
    //
    // - For the repointed dependent we still need to collect its prerequisite
    //   replacements to make sure its dependency constraints are satisfied.
    //
    // - If configuration variables are specified for the dependent which has
    //   any buildfile clauses in the dependencies, then we need to
    //   re-evaluate them. This can result in a different set of dependencies
    //   required by this dependent (due to conditional dependencies, etc)
    //   and, potentially, for its reconfigured existing prerequisites,
    //   recursively.
    //
    // - For an existing dependent being re-evaluated to the specific
    //   dependency position (reeval_pos argument is specified and is not
    //   {0,0}).
    //
    // - For an existing dependent being pre-reevaluated (reeval_pos argument
    //   is {0,0}).
    //
    // - For an existing dependent being re-collected due to the selected
    //   dependency alternatives deviation, etc which may be caused by its
    //   dependency up/downgrade (see postponed_packages and
    //   build_package::build_recollect flag for details).
    //
    // Note that for these cases, as it was said above, we can potentially
    // fail if the dependent is an orphan, but this is exactly what we need to
    // do in that case, since we won't be able to re-collect its dependencies.
    //
    // Only a single true dependency alternative can be selected per function
    // call, unless we are (pre-)re-evaluating. Such an alternative can only
    // be selected if its index in the postponed alternatives list is less
    // than the specified maximum (used by the heuristics that determines in
    // which order to process packages with alternatives; if 0 is passed, then
    // no true alternative will be selected).
    //
    // The idea here is to postpone the true alternatives selection till the
    // end of the packages collection and then try to optimize the overall
    // resulting selection (over all the dependents) by selecting alternatives
    // with the lower indexes first (see collect_build_postponed() for
    // details).
    //
    // Always postpone recursive collection of dependencies for a dependent
    // with configuration clauses, recording them together with the dependent
    // in postponed_cfgs (see postponed_configurations for details). If it
    // turns out that some dependency of such a dependent has already been
    // collected via some other dependent without configuration clauses, then
    // record it in postponed_deps and throw the postpone_dependency
    // exception. This exception is handled via re-collecting packages from
    // scratch, but now with the knowledge about premature dependency
    // collection. If some dependency already belongs to some non or being
    // negotiated cluster then throw merge_configuration. If some dependencies
    // have existing dependents with config clauses which have not been
    // considered for the configuration negotiation yet, then throw
    // recollect_existing_dependents exception to re-collect these dependents.
    // If configuration has already been negotiated between some other
    // dependents, then up-negotiate the configuration and throw
    // retry_configuration exception so that the configuration refinement can
    // be performed. See the collect lambda implementation for details on the
    // configuration refinement machinery.
    //
    // If the reeval_pos argument is specified and is not {0,0}, then
    // re-evaluate the package to the specified position. In this mode perform
    // the regular dependency alternative selection and non-recursive
    // dependency collection. When the specified position is reached, postpone
    // the collection by recording the dependent together with the
    // dependencies at that position in postponed_cfgs (see
    // postponed_configurations for details). If the dependent/dependencies
    // are added to an already negotiated cluster, then throw
    // merge_configuration, similar to the regular collection mode (see
    // above). Also check for the merge configuration cycles (see the function
    // implementation for details) and throw the merge_configuration_cycle
    // exception if such a cycle is detected.
    //
    // If {0,0} is specified as the reeval_pos argument, then perform the
    // pre-reevaluation of an existing dependent, requested due to the
    // specific dependency up/down-grade or reconfiguration (must be passed as
    // the orig_dep; we call it originating dependency). The main purpose of
    // this read-only mode is to obtain the position of the earliest selected
    // dependency alternative with the config clause, if any, which the
    // re-evaluation needs to be performed to and to determine if such a
    // re-evaluation is optional (see pre_reevaluate_result for the full
    // information being retrieved). The re-evaluation is considered to be
    // optional if the existing dependent has no config clause for the
    // originating dependency and the enable and reflect clauses do not refer
    // to any of the dependency configuration variables (which can only be
    // those which the dependent has the configuration clauses for; see the
    // bpkg manual for details). The thinking here is that such an existing
    // dependent may not change any configuration it applies to its
    // dependencies and thus it doesn't call for any negotiations (note: if
    // there are config clauses for the upgraded originating dependency, then
    // the potentially different defaults for its config variables may affect
    // the configuration this dependent applies to its dependencies). Such a
    // dependent can also be reconfigured without pre-selection of its
    // dependency alternatives since pkg-configure is capable of doing that on
    // its own for such a simple case (see pkg_configure_prerequisites() for
    // details). Also look for any deviation in the dependency alternatives
    // selection and throw reevaluation_deviated exception if such a deviation
    // is detected. Return nullopt if no dependency alternative with the
    // config clause is selected.
    //
    // If the package is a dependency of configured dependents and needs to be
    // reconfigured (being upgraded, has configuration specified, etc), then
    // do the following for each such dependent prior to collecting its own
    // prerequisites:
    //
    // - If the dependent is not already being built/dropped, expected to be
    //   built/dropped, and doesn't apply constraints which the dependency
    //   doesn't satisfy anymore, then pre-reevaluate the dependent.
    //
    // - If the dependency alternative with configuration clause has been
    //   encountered during the pre-reevaluation, then record it in
    //   postponed_cfgs as a single-dependency cluster with an existing
    //   dependent (see postponed_configurations for details). If the index of
    //   the encountered depends clause is equal/less than the index of the
    //   depends clause the dependency belongs to, then postpone the recursive
    //   collection of this dependency assuming that it will be collected
    //   later, during/after its existing dependent re-evaluation.
    //
    // - If the dependency alternatives selection has deviated, then record
    //   the dependent in postponed_recs (so that it can be re-collected
    //   later) and postpone recursive collection of this dependency assuming
    //   that it will be collected later, during its existing dependent
    //   re-collection. Also record this dependency in the postponed existing
    //   dependencies map (postponed_existing_dependencies argument). This way
    //   the caller can track if the postponed dependencies have never been
    //   collected recursively (deviations are too large, etc) and handle this
    //   situation (currently just fail).
    //
    // If a dependency alternative configuration cannot be negotiated between
    // all the dependents, then unaccept_alternative can be thrown (see
    // unacceptable_alternatives for details).
    //
    struct postpone_dependency: scratch_collection
    {
      package_key package;

      explicit
      postpone_dependency (package_key p)
          : scratch_collection ("prematurely collected dependency"),
            package (move (p))
      {
        scratch_collection::package = &package;
      }
    };

    struct retry_configuration
    {
      size_t      depth;
      package_key dependent;
    };

    struct merge_configuration
    {
      size_t depth;
    };

    struct merge_configuration_cycle
    {
      size_t depth;
    };

    struct reevaluation_deviated {};

    struct pre_reevaluate_result
    {
      using packages = postponed_configuration::packages;

      pair<size_t, size_t> reevaluation_position;
      packages             reevaluation_dependencies;
      bool                 reevaluation_optional = true;
      pair<size_t, size_t> originating_dependency_position;
    };

    optional<pre_reevaluate_result>
    collect_build_prerequisites (const pkg_build_options&,
                                 build_package&,
                                 build_package_refs& dep_chain,
                                 const function<find_database_function>&,
                                 const function<add_priv_cfg_function>&,
                                 const repointed_dependents&,
                                 replaced_versions&,
                                 postponed_packages* postponed_repo,
                                 postponed_packages* postponed_alts,
                                 size_t max_alt_index,
                                 postponed_packages& postponed_recs,
                                 postponed_existing_dependencies&,
                                 postponed_dependencies&,
                                 postponed_configurations&,
                                 unacceptable_alternatives&,
                                 unsatisfied_dependents&,
                                 optional<pair<size_t, size_t>> reeval_pos = nullopt,
                                 const optional<package_key>& orig_dep = nullopt);

    void
    collect_build_prerequisites (const pkg_build_options&,
                                 database&,
                                 const package_name&,
                                 const function<find_database_function>&,
                                 const function<add_priv_cfg_function>&,
                                 const repointed_dependents&,
                                 replaced_versions&,
                                 postponed_packages& postponed_repo,
                                 postponed_packages& postponed_alts,
                                 size_t max_alt_index,
                                 postponed_packages& postponed_recs,
                                 postponed_existing_dependencies&,
                                 postponed_dependencies&,
                                 postponed_configurations&,
                                 unacceptable_alternatives&,
                                 unsatisfied_dependents&);

    // Collect the repointed dependents and their replaced prerequisites,
    // recursively.
    //
    // If a repointed dependent is already pre-entered or collected with an
    // action other than adjustment, then just mark it for reconfiguration
    // unless it is already implied. Otherwise, collect the package build with
    // the repoint sub-action and reconfigure adjustment flag.
    //
    void
    collect_repointed_dependents (const pkg_build_options&,
                                  const repointed_dependents&,
                                  replaced_versions&,
                                  postponed_packages& postponed_repo,
                                  postponed_packages& postponed_alts,
                                  postponed_packages& postponed_recs,
                                  postponed_existing_dependencies&,
                                  postponed_dependencies&,
                                  postponed_configurations&,
                                  unacceptable_alternatives&,
                                  unsatisfied_dependents&,
                                  const function<find_database_function>&,
                                  const function<add_priv_cfg_function>&);

    // Collect the package being dropped. Noop if the specified package is
    // already being built and its required-by package names have the
    // "required by dependents" semantics.
    //
    // Add entry to replaced_vers and throw replace_version if the existing
    // version needs to be dropped but this can't be done in-place (see
    // replaced_versions for details).
    //
    void
    collect_drop (const pkg_build_options&,
                  database&,
                  shared_ptr<selected_package>,
                  replaced_versions&);

    // Collect the package being unheld.
    //
    void
    collect_unhold (database&, const shared_ptr<selected_package>&);

    void
    collect_build_postponed (const pkg_build_options&,
                             replaced_versions&,
                             postponed_packages& postponed_repo,
                             postponed_packages& postponed_alts,
                             postponed_packages& postponed_recs,
                             postponed_existing_dependencies&,
                             postponed_dependencies&,
                             postponed_configurations&,
                             strings& postponed_cfgs_history,
                             unacceptable_alternatives&,
                             unsatisfied_dependents&,
                             const function<find_database_function>&,
                             const repointed_dependents&,
                             const function<add_priv_cfg_function>&,
                             postponed_configuration* = nullptr);

    // If a configured package is being up/down-graded or reconfigured then
    // that means all its configured dependents could be affected and we have
    // to reconfigure them. This function examines every such a package that
    // is already in the map and collects all its configured dependents. We
    // also need to make sure the dependents are ok with the up/downgrade. If
    // some dependency constraints are not satisfied, then cache them and
    // proceed further as if no problematic constraints are imposed (see
    // unsatisfied_dependents for details). Return the set of the collected
    // dependents.
    //
    // Should we reconfigure just the direct depends or also include indirect,
    // recursively? Consider this plausible scenario as an example: We are
    // upgrading a package to a version that provides an additional API. When
    // its direct dependent gets reconfigured, it notices this new API and
    // exposes its own extra functionality that is based on it. Now it would
    // make sense to let its own dependents (which would be our original
    // package's indirect ones) to also notice this.
    //
    std::set<package_key>
    collect_dependents (const repointed_dependents&, unsatisfied_dependents&);

    // Order the previously-collected package with the specified name and
    // configuration returning its position.
    //
    // Recursively order the collected package dependencies, failing if a
    // dependency cycle is detected. If reorder is true, then reorder this
    // package to be considered as "early" as possible.
    //
    iterator
    order (database&,
           const package_name&,
           const function<find_database_function>&,
           bool reorder = true);

    void
    clear ();

    void
    clear_order ();

    // Print all the version constraints (one per line) applied to this
    // package and its dependents, recursively. The specified package is
    // expected to be collected (present in the map). Don't print the version
    // constraints for the same package twice, printing "..." instead. Noop if
    // there are no constraints for this package.
    //
    // Optionally, only print constraints from the existing or being built
    // dependents (see build_package::constraint_type for details).
    //
    void
    print_constraints (diag_record&,
                       const build_package&,
                       string& indent,
                       std::set<package_key>& printed,
                       optional<bool> existing_dependent = nullopt) const;

    void
    print_constraints (diag_record&,
                       const package_key&,
                       string& indent,
                       std::set<package_key>& printed,
                       optional<bool> existing_dependent = nullopt) const;

    // Verify that builds ordering is consistent across all the data
    // structures and the ordering expectations are fulfilled (real build
    // actions are all ordered, etc).
    //
    void
    verify_ordering () const;

  private:
    // Return the list of existing dependents that has a configuration clause
    // for any of the selected alternatives together with the dependencies for
    // the earliest such an alternative and the originating dependency (for
    // which the function is called for) position. Return absent dependency
    // for those dependents which dependency alternatives selection has
    // deviated (normally due to the dependency up/downgrade). Skip dependents
    // which are being built and require recursive recollection or dropped
    // (present in the map) or expected to be built or dropped (present in
    // rpt_depts or replaced_vers). Also skip dependents which impose the
    // version constraint on this dependency and the dependency doesn't
    // satisfy this constraint. Optionally, skip the existing dependents for
    // which re-evaluation is considered optional (exclude_optional argument;
    // see pre-reevaluation mode of collect_build_prerequisites() for
    // details).
    //
    // Note that the originating dependency is expected to be collected
    // (present in the map).
    //
    struct existing_dependent
    {
      // Dependent.
      //
      reference_wrapper<database>  db;
      shared_ptr<selected_package> selected;

      // Earliest dependency with config clause.
      //
      optional<package_key>        dependency;
      pair<size_t, size_t>         dependency_position;

      // Originating dependency passed to the function call.
      //
      package_key                  originating_dependency;
      pair<size_t, size_t>         originating_dependency_position;
    };

    // This exception is thrown by collect_build_prerequisites() and
    // collect_build_postponed() to resolve different kinds of existing
    // dependent re-evaluation related cycles by re-collecting the problematic
    // dependents from scratch.
    //
    struct recollect_existing_dependents
    {
      size_t depth;
      vector<existing_dependent> dependents;
    };

    vector<existing_dependent>
    query_existing_dependents (
      tracer&,
      const pkg_build_options&,
      database&,
      const package_name&,
      bool exclude_optional,
      const function<find_database_function>&,
      const repointed_dependents&,
      const replaced_versions&);

    // Non-recursively collect the dependency of an existing dependent
    // previously returned by the query_existing_dependents() function call
    // with the build_package::build_reevaluate flag.
    //
    const build_package*
    collect_existing_dependent_dependency (
      const pkg_build_options&,
      const existing_dependent&,
      replaced_versions&,
      postponed_configurations&,
      unsatisfied_dependents&);

    // Non-recursively collect an existing non-deviated dependent previously
    // returned by the query_existing_dependents() function call for the
    // subsequent re-evaluation.
    //
    void
    collect_existing_dependent (
      const pkg_build_options&,
      const existing_dependent&,
      postponed_configuration::packages&& dependencies,
      replaced_versions&,
      postponed_configurations&,
      unsatisfied_dependents&);

    // Non-recursively collect an existing dependent previously returned by
    // the query_existing_dependents() function call with the
    // build_package::build_recollect flag and add it to the postponed package
    // recollections list. Also add the build_package::adjust_reconfigure flag
    // for the deviated dependents (existing_dependent::dependency is absent).
    //
    // Note that after this function call the existing dependent may not be
    // returned as a result by the query_existing_dependents() function
    // anymore (due to the build_package::build_recollect flag presence).
    //
    void
    recollect_existing_dependent (const pkg_build_options&,
                                  const existing_dependent&,
                                  replaced_versions&,
                                  postponed_packages& postponed_recs,
                                  postponed_configurations&,
                                  unsatisfied_dependents&,
                                  bool add_required_by);

    // Skip the dependents collection for the specified dependency if that has
    // already been done.
    //
    // Note that if this function has already been called for this dependency,
    // then all its dependents are already in the map and their dependency
    // constraints have been checked.
    //
    void
    collect_dependents (build_package&,
                        const repointed_dependents&,
                        unsatisfied_dependents&,
                        std::set<const build_package*>& visited_deps,
                        std::set<package_key>& result);

    struct package_ref
    {
      database& db;
      const package_name& name;

      bool
      operator== (const package_ref&);
    };
    using package_refs = small_vector<package_ref, 16>;

    iterator
    order (database&,
           const package_name&,
           package_refs& chain,
           const function<find_database_function>&,
           bool reorder);

  private:
    struct data_type
    {
      iterator position;         // Note: can be end(), see collect_build().
      build_package package;
    };

    class package_map: public std::map<package_key, data_type>
    {
    public:
      using base_type = std::map<package_key, data_type>;

      using base_type::find;

      iterator
      find (database& db, const package_name& pn)
      {
        return find (package_key {db, pn});
      }

      const_iterator
      find (database& db, const package_name& pn) const
      {
        return find (package_key {db, pn});
      }

      // Try to find a package build in the dependency configurations (see
      // database::dependency_configs() for details). Return the end iterator
      // if no build is found and issue diagnostics and fail if multiple
      // builds (in multiple configurations) are found.
      //
      iterator
      find_dependency (database&, const package_name&, bool buildtime);
    };
    package_map map_;
  };
}

#endif // BPKG_PKG_BUILD_COLLECT_HXX
