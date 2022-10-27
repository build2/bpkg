// file      : bpkg/pkg-build-collect.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_BUILD_COLLECT_HXX
#define BPKG_PKG_BUILD_COLLECT_HXX

#include <map>
#include <set>
#include <list>
#include <forward_list>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // database, linked_databases
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>
#include <bpkg/diagnostics.hxx>

#include <bpkg/common-options.hxx>
#include <bpkg/pkg-build-options.hxx>

#include <bpkg/pkg-configure.hxx>    // find_database_function()
#include <bpkg/package-skeleton.hxx>

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
    std::set<package_key> required_by;

    // If this flags is true, then required_by contains dependents.
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
    // collected.
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
                   const shared_ptr<available_package>& override = nullptr);
  };

  using build_package_list = std::list<reference_wrapper<build_package>>;

  using build_package_refs =
    small_vector<reference_wrapper<const build_package>, 16>;

  // Packages with postponed prerequisites collection, for one of the
  // following reasons:
  //
  // - Postponed due to the inability to find a version satisfying the pre-
  //   entered constraint from repositories available to this package. The
  //   idea is that this constraint could still be satisfied from a repository
  //   fragment of some other package (that we haven't processed yet) that
  //   also depends on this prerequisite.
  //
  // - Postponed due to the inability to choose between two dependency
  //   alternatives, both having dependency packages which are not yet
  //   selected in the configuration nor being built. The idea is that this
  //   ambiguity could still be resolved after some of those dependency
  //   packages get built via some other dependents.
  //
  using postponed_packages = std::set<build_package*>;

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
  // (without config) but not the second (with config). We also need to track
  // at which phase of collection an entry has been added to process the bogus
  // entries accordingly.
  //
  struct postponed_dependency
  {
    bool wout_config; // Has dependent without config.
    bool with_config; // Has dependent with config.
    bool initial_collection;

    postponed_dependency (bool woc, bool wic, bool ic)
        : wout_config (woc),
          with_config (wic),
          initial_collection (ic) {}

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
    cancel_bogus (tracer& trace, bool initial_collection)
    {
      bool bogus (false);
      for (auto i (begin ()); i != end (); )
      {
        const postponed_dependency& d (i->second);

        if (d.bogus () && (!initial_collection || d.initial_collection))
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

  // Map of existing dependents which may not be re-evaluated to a position
  // with the dependency index greater than the specified one.
  //
  // This mechanism applies when we re-evaluate an existing dependent to a
  // certain position but later realize we've gone too far. In this case we
  // note the earlier position information and re-collect from scratch. On the
  // re-collection any re-evaluation of the dependent to a greater position
  // will be either skipped or performed but to this earlier position (see the
  // replace member for details).
  //
  // We consider the postponement bogus if some dependent re-evaluation was
  // skipped due to its presence but no re-evaluation to this (or earlier)
  // dependency index was performed. Thus, if after the collection of packages
  // some bogus entries are present in the map, then it means that we have
  // skipped the respective re-evaluations erroneously and so need to erase
  // these entries and re-collect.
  //
  // Note that if no re-evaluation is skipped due to a postponement then it
  // is harmless and we don't consider it bogus.
  //
  struct postponed_position: pair<size_t, size_t>
  {
    // True if the "later" position should be replaced rather than merely
    // skipped. The replacement deals with the case where the "earlier"
    // position is encountered while processing the same cluster as what
    // contains the later position. In this case, if we merely skip, then we
    // will never naturally encounter the earlier position. So we have to
    // force the issue (even if things change enough for us to never see the
    // later position again).
    //
    bool replace;

    // Re-evaluation was skipped due to this postponement.
    //
    bool skipped = false;

    // The dependent was re-evaluated. Note that it can be only re-evaluated
    // to this or earlier dependency index.
    //
    bool reevaluated = false;

    postponed_position (pair<size_t, size_t> p, bool r)
        : pair<size_t, size_t> (p), replace (r) {}
  };

  class postponed_positions: public std::map<package_key, postponed_position>
  {
  public:
    // If true, override the first encountered non-replace position to replace
    // and clear this flag. See collect_build_postponed() for details.
    //
    bool replace = false;

    // Erase the bogus postponements and throw cancel_postponement, if any.
    //
    struct cancel_postponement: scratch_collection
    {
      cancel_postponement ()
          : scratch_collection ("bogus existing dependent re-evaluation "
                                "postponement cancellation") {}
    };

    void
    cancel_bogus (tracer& trace)
    {
      bool bogus (false);
      for (auto i (begin ()); i != end (); )
      {
        const postponed_position& p (i->second);

        if (p.skipped && !p.reevaluated)
        {
          bogus = true;

          l5 ([&]{trace << "erase bogus existing dependent " << i->first
                        << " re-evaluation postponement with dependency index "
                        << i->second.first;});

          // It seems that the replacement may never be bogus.
          //
          assert (!p.replace);

          i = erase (i);
        }
        else
          ++i;
      }

      if (bogus)
      {
        l5 ([&]{trace << "bogus re-evaluation postponement erased, throwing";});
        throw cancel_postponement ();
      }
    }
  };

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
    // merged into it.
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

    // If the configuration contains the specified existing dependent, then
    // return the earliest dependency position. Otherwise return NULL.
    //
    const pair<size_t, size_t>*
    existing_dependent_position (const package_key&) const;

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
    operator= (build_packages&&);

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

    // Collect the package being built. Return its pointer if this package
    // version was, in fact, added to the map and NULL if it was already there
    // and the existing version was preferred or if the package build has been
    // replaced with the drop. So can be used as bool.
    //
    // Consult replaced_vers for an existing version replacement entry and
    // follow it, if present, potentially collecting the package drop
    // instead. Add entry to replaced_vers and throw replace_version if the
    // existing version needs to be replaced but the new version cannot be
    // re-collected recursively in-place (see replaced_versions for details).
    // Also add an entry and throw if the existing dependent needs to be
    // replaced.
    //
    // Optionally, pass the function which verifies the chosen package
    // version. It is called before replace_version is potentially thrown or
    // the recursive collection is performed. The scratch argument is true if
    // the package version needs to be replaced but in-place replacement is
    // not possible (see replaced_versions for details).
    //
    // Also, in the recursive mode (dep_chain is not NULL):
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
    // Note that postponed_* and dep_chain arguments must all be either
    // specified or not.
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
                   const function<find_database_function>&,
                   const repointed_dependents&,
                   const function<add_priv_cfg_function>&,
                   bool initial_collection,
                   replaced_versions&,
                   postponed_configurations&,
                   build_package_refs* dep_chain = nullptr,
                   postponed_packages* postponed_repo = nullptr,
                   postponed_packages* postponed_alts = nullptr,
                   postponed_dependencies* = nullptr,
                   postponed_positions* = nullptr,
                   unacceptable_alternatives* = nullptr,
                   const function<verify_package_build_function>& = nullptr);

    // Collect prerequisites of the package being built recursively.
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
    //   dependency position.
    //
    // Note that for these cases, as it was said above, we can potentially
    // fail if the dependent is an orphan, but this is exactly what we need to
    // do in that case, since we won't be able to re-collect its dependencies.
    //
    // Only a single true dependency alternative can be selected per function
    // call. Such an alternative can only be selected if its index in the
    // postponed alternatives list is less than the specified maximum (used by
    // the heuristics that determines in which order to process packages with
    // alternatives; if 0 is passed, then no true alternative will be
    // selected).
    //
    // The idea here is to postpone the true alternatives selection till the
    // end of the packages collection and then try to optimize the overall
    // resulting selection (over all the dependents) by selecting alternatives
    // with the lower indexes first (see collect_build_postponed() for
    // details).
    //
    // Always postpone recursive collection of dependencies for a dependent
    // with configuration clauses, recording them in postponed_deps (see
    // postponed_dependencies for details) and also recording the dependent in
    // postponed_cfgs (see postponed_configurations for details). If it turns
    // out that some dependency of such a dependent has already been collected
    // via some other dependent without configuration clauses, then throw the
    // postpone_dependency exception. This exception is handled via
    // re-collecting packages from scratch, but now with the knowledge about
    // premature dependency collection. If some dependency already belongs to
    // some non or being negotiated cluster then throw merge_configuration.
    // If some dependency configuration has already been negotiated between
    // some other dependents, then up-negotiate the configuration and throw
    // retry_configuration exception so that the configuration refinement can
    // be performed. See the collect lambda implementation for details on the
    // configuration refinement machinery.
    //
    // If the package is a dependency of a configured dependent with
    // configuration clause and needs to be reconfigured (being upgraded, has
    // configuration specified, etc), then postpone its recursive collection
    // by recording it in postponed_cfgs as a single-dependency cluster with
    // an existing dependent (see postponed_configurations for details). If
    // this dependent already belongs to some (being) negotiated configuration
    // cluster with a greater dependency position then record this dependency
    // position in postponed_poss and throw postpone_position. This exception
    // is handled by re-collecting packages from scratch, but now with the
    // knowledge about position this dependent needs to be re-evaluated to.
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

    struct postpone_position: scratch_collection
    {
      postpone_position ()
          : scratch_collection ("earlier dependency position") {}
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

    void
    collect_build_prerequisites (const pkg_build_options&,
                                 build_package&,
                                 const function<find_database_function>&,
                                 const repointed_dependents&,
                                 const function<add_priv_cfg_function>&,
                                 bool initial_collection,
                                 replaced_versions&,
                                 build_package_refs& dep_chain,
                                 postponed_packages* postponed_repo,
                                 postponed_packages* postponed_alts,
                                 size_t max_alt_index,
                                 postponed_dependencies&,
                                 postponed_configurations&,
                                 postponed_positions&,
                                 unacceptable_alternatives&,
                                 pair<size_t, size_t> reeval_pos =
                                 make_pair(0, 0));

    void
    collect_build_prerequisites (const pkg_build_options&,
                                 database&,
                                 const package_name&,
                                 const function<find_database_function>&,
                                 const repointed_dependents&,
                                 const function<add_priv_cfg_function>&,
                                 bool initial_collection,
                                 replaced_versions&,
                                 postponed_packages& postponed_repo,
                                 postponed_packages& postponed_alts,
                                 size_t max_alt_index,
                                 postponed_dependencies&,
                                 postponed_configurations&,
                                 postponed_positions&,
                                 unacceptable_alternatives&);

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
                                  postponed_dependencies&,
                                  postponed_configurations&,
                                  postponed_positions&,
                                  unacceptable_alternatives&,
                                  const function<find_database_function>&,
                                  const function<add_priv_cfg_function>&);

    // Collect the package being dropped.
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
                             postponed_dependencies&,
                             postponed_configurations&,
                             strings& postponed_cfgs_history,
                             postponed_positions&,
                             unacceptable_alternatives&,
                             const function<find_database_function>&,
                             const repointed_dependents&,
                             const function<add_priv_cfg_function>&,
                             postponed_configuration* = nullptr);

    // Order the previously-collected package with the specified name
    // returning its positions.
    //
    // If buildtime is nullopt, then search for the specified package build in
    // only the specified configuration. Otherwise, treat the package as a
    // dependency and use the custom search function to find its build
    // configuration. Failed that, search for it recursively (see
    // package_map::find_dependency() for details).
    //
    // Recursively order the package dependencies being ordered failing if a
    // dependency cycle is detected. If reorder is true, then reorder this
    // package to be considered as "early" as possible.
    //
    iterator
    order (database&,
           const package_name&,
           optional<bool> buildtime,
           const function<find_database_function>&,
           bool reorder = true);

    // If a configured package is being up/down-graded then that means all its
    // dependents could be affected and we have to reconfigure them. This
    // function examines every package that is already on the list and collects
    // and orders all its dependents. We also need to make sure the dependents
    // are ok with the up/downgrade.
    //
    // Should we reconfigure just the direct depends or also include indirect,
    // recursively? Consider this plauisible scenario as an example: We are
    // upgrading a package to a version that provides an additional API. When
    // its direct dependent gets reconfigured, it notices this new API and
    // exposes its own extra functionality that is based on it. Now it would
    // make sense to let its own dependents (which would be our original
    // package's indirect ones) to also notice this.
    //
    void
    collect_order_dependents (const repointed_dependents&);

    void
    collect_order_dependents (iterator, const repointed_dependents&);

    void
    clear ();

    void
    clear_order ();

    // Verify that builds ordering is consistent across all the data
    // structures and the ordering expectations are fulfilled (real build
    // actions are all ordered, etc).
    //
    void
    verify_ordering () const;

  private:
    // Return the list of existing dependents that has a configuration clause
    // for the specified dependency. Skip dependents which are being built and
    // require recursive recollection or dropped (present in the map) or
    // expected to be built or dropped (present in rpt_depts or replaced_vers).
    //
    // Optionally, specify the function which can verify the dependent build
    // and decide whether to override the default behavior and still add the
    // dependent package to the resulting list, returning true in this case.
    //
    struct existing_dependent
    {
      reference_wrapper<database>   db;
      shared_ptr<selected_package>  selected;
      pair<size_t, size_t>          dependency_position;
    };

    using verify_dependent_build_function = bool (const package_key&,
                                                  pair<size_t, size_t>);

    vector<existing_dependent>
    query_existing_dependents (
      tracer&,
      database&,
      const package_name&,
      const replaced_versions&,
      const repointed_dependents&,
      const function<verify_dependent_build_function>& = nullptr);

    // Update the existing dependent object (previously obtained with the
    // query_existing_dependents() call) with the new dependency position and
    // collect the dependency referred by this position. Return the pointer to
    // the collected build package object.
    //
    const build_package*
    replace_existing_dependent_dependency (
      tracer&,
      const pkg_build_options&,
      existing_dependent&,
      pair<size_t, size_t>,
      const function<find_database_function>&,
      const repointed_dependents&,
      const function<add_priv_cfg_function>&,
      bool initial_collection,
      replaced_versions&,
      postponed_configurations&);

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
           optional<bool> buildtime,
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