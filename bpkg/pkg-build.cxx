// file      : bpkg/pkg-build.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-build.hxx>

#include <map>
#include <set>
#include <list>
#include <cstring>    // strlen()
#include <iostream>   // cout

#include <libbutl/standard-version.mxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/satisfaction.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/common-options.hxx>

#include <bpkg/pkg-purge.hxx>
#include <bpkg/pkg-fetch.hxx>
#include <bpkg/rep-fetch.hxx>
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

  // Try to find packages that optionally satisfy the specified version
  // constraint. Return the list of packages and repository fragments in which
  // each was found or empty list if none were found. Note that a stub
  // satisfies any constraint.
  //
  static
  vector<pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>>
  find_available (database& db,
                  const package_name& name,
                  const optional<version_constraint>& c)
  {
    vector<pair<shared_ptr<available_package>,
                shared_ptr<repository_fragment>>> r;

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
      r.emplace_back (move (ap),
                      ap->locations[0].repository_fragment.load ());
    }

    // Adding a stub from the imaginary system repository to the non-empty
    // results isn't necessary but may end up with a duplicate. That's why we
    // only add it if nothing else is found.
    //
    if (r.empty ())
    {
      shared_ptr<available_package> ap (find_imaginary_stub (name));

      if (ap != nullptr)
        r.emplace_back (move (ap), nullptr);
    }

    return r;
  }

  // As above but only look for packages from the specified list fo repository
  // fragments, their prerequisite repositories, and their complements,
  // recursively (note: recursivity applies to complements, not
  // prerequisites).
  //
  static
  vector<pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>>
  find_available (database& db,
                  const package_name& name,
                  const optional<version_constraint>& c,
                  const vector<shared_ptr<repository_fragment>>& rfs,
                  bool prereq = true)
  {
    // Filter the result based on the repository fragments to which each
    // version belongs.
    //
    vector<pair<shared_ptr<available_package>,
                shared_ptr<repository_fragment>>> r (
                  filter (rfs, query_available (db, name, c), prereq));

    if (r.empty ())
    {
      shared_ptr<available_package> ap (find_imaginary_stub (name));

      if (ap != nullptr)
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
  static pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>
  find_available_one (database& db,
                      const package_name& name,
                      const optional<version_constraint>& c,
                      const shared_ptr<repository_fragment>& rf,
                      bool prereq = true)
  {
    // Filter the result based on the repository fragment to which each
    // version belongs.
    //
    auto r (filter_one (rf, query_available (db, name, c), prereq));

    if (r.first == nullptr)
      r.first = find_imaginary_stub (name);

    return r;
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
  static pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>
  make_available (const common_options& options,
                  const dir_path& c,
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
                    a->absolute () ? *a : c / *a,
                    true /* ignore_unknown */,
                    false /* expand_values */)
      : pkg_verify (sp->effective_src_root (c),
                    true /* ignore_unknown */,
                    // Copy potentially fixed up version from selected package.
                    [&sp] (version& v) {v = sp->version;}));

    return make_pair (make_shared<available_package> (move (m)), move (af));
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

    shared_ptr<selected_package>  selected;   // NULL if not selected.
    shared_ptr<available_package> available;  // Can be NULL, fake/transient.

    // Can be NULL (orphan) or root.
    //
    shared_ptr<bpkg::repository_fragment> repository_fragment;

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
      string dependent;
      version_constraint value;

      constraint_type () = default;
      constraint_type (string d, version_constraint v)
          : dependent (move (d)), value (move (v)) {}
    };

    vector<constraint_type> constraints;

    // System package indicator. See also a note in the merge() function.
    //
    bool system;

    // If the flag is set and the external package is being replaced with an
    // external one, then keep its output directory between upgrades and
    // downgrades.
    //
    bool keep_out;

    // Command line configuration variables. Only meaningful for non-system
    // packages.
    //
    strings config_vars;

    // Set of package names that caused this package to be built or adjusted.
    // Empty name signifies user selection.
    //
    set<package_name> required_by;

    bool
    user_selection () const
    {
      return required_by.find (package_name ()) != required_by.end ();
    }

    // Adjustment flags.
    //
    uint16_t adjustments;

    // Set if we also need to clear the hold package flag.
    //
    static const uint16_t adjust_unhold = 0x0001;

    bool
    unhold () const
    {
      return (adjustments & adjust_unhold) != 0;
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
             ((adjustments & adjust_reconfigure) != 0 ||
              (*action == build &&
               (selected->system () != system             ||
                selected->version != available_version () ||
                (!system && !config_vars.empty ()))));
    }

    const version&
    available_version () const
    {
      // This should have been diagnosed before creating build_package object.
      //
      assert (available != nullptr &&
              (system
               ? available->system_version () != nullptr
               : !available->stub ()));

      return system ? *available->system_version () : available->version;
    }

    string
    available_name_version () const
    {
      assert (available != nullptr);
      return package_string (available->id.name, available_version (), system);
    }

    // Merge constraints, required-by package names, hold_* flags,
    // adjustments, and user-specified options/variables.
    //
    void
    merge (build_package&& p)
    {
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
        // the iterative plan refinement implementation details (variables are
        // only saved into the pre-entered dependencies, etc.).
        //
        if (p.keep_out)
          keep_out = p.keep_out;

        if (!p.config_vars.empty ())
          config_vars = move (p.config_vars);

        // Propagate the user-selection tag.
        //
        required_by.insert (package_name ());
      }

      // Required-by package names have different semantics for different
      // actions: dependent for builds and prerequisite for adjustment. Mixing
      // them would break prompts/diagnostics, so we copy them only if actions
      // match.
      //
      if (p.action && *p.action == *action)
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

      // Copy adjustments flags.
      //
      adjustments |= p.adjustments;

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

      auto p (map_.emplace (move (name), data_type {end (), move (pkg)}));
      assert (p.second);
    }

    // Collect the package being built. Return its pointer if this package
    // version was, in fact, added to the map and NULL if it was already there
    // or the existing version was preferred. So can be used as bool.
    //
    build_package*
    collect_build (const common_options& options,
                   const dir_path& cd,
                   database& db,
                   build_package pkg,
                   postponed_packages* recursively = nullptr)
    {
      using std::swap; // ...and not list::swap().

      tracer trace ("collect_build");

      // Only builds are allowed here.
      //
      assert (pkg.action && *pkg.action == build_package::build &&
              pkg.available != nullptr);

      auto i (map_.find (pkg.available->id.name));

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

          if (p1->available_version () != p2->available_version ())
          {
            using constraint_type = build_package::constraint_type;

            // If the versions differ, we have to pick one. Start with the
            // newest version since if both satisfy, then that's the one we
            // should prefer. So get the first to try into p1 and the second
            // to try -- into p2.
            //
            if (p2->available_version () > p1->available_version ())
              swap (p1, p2);

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
                const package_name& n  (i->first);
                const string&       d1 (c1->dependent);
                const string&       d2 (c2->dependent);

                fail << "unable to satisfy constraints on package " << n <<
                  info << d1 << " depends on (" << n << " " << c1->value
                     << ")" <<
                  info << d2 << " depends on (" << n << " " << c2->value
                       << ")" <<
                  info << "available " << p1->available_name_version () <<
                  info << "available " << p2->available_name_version () <<
                  info << "explicitly specify " << n << " version to manually "
                       << "satisfy both constraints";
              }
              else
                swap (p1, p2);
            }

            l4 ([&]{trace << "pick " << p1->available_name_version ()
                          << " over " << p2->available_name_version ();});
          }
          // If versions are the same, then we still need to pick the entry as
          // one of them can build a package from source while another
          // configure a system package. We prefer a user-selected entry (if
          // there is one). If none of them is user-selected we prefer a
          // source package over a system one.
          //
          else if (p2->user_selection () ||
                   (!p1->user_selection () && !p2->system))
            swap (p1, p2);

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
        l4 ([&]{trace << "add " << pkg.available_name_version ();});

        // Note: copy; see emplace() below.
        //
        package_name n (pkg.available->id.name);
        i = map_.emplace (move (n), data_type {end (), move (pkg)}).first;
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
        collect_build_prerequisites (options, cd, db, p, recursively);

      return &p;
    }

    // Collect prerequisites of the package being built recursively. But first
    // "prune" this process if the package we build is a system one or is
    // already configured since that would mean all its prerequisites are
    // configured as well. Note that this is not merely an optimization: the
    // package could be an orphan in which case the below logic will fail (no
    // repository fragment in which to search for prerequisites). By skipping
    // the prerequisite check we are able to gracefully handle configured
    // orphans.
    //
    void
    collect_build_prerequisites (const common_options& options,
                                 const dir_path& cd,
                                 database& db,
                                 const build_package& pkg,
                                 postponed_packages* postponed)
    {
      tracer trace ("collect_build_prerequisites");

      assert (pkg.action && *pkg.action == build_package::build);

      const shared_ptr<selected_package>& sp (pkg.selected);

      if (pkg.system ||
          (sp != nullptr &&
           sp->state == package_state::configured &&
           sp->substate != package_substate::system &&
           sp->version == pkg.available_version ()))
        return;

      // Show how we got here if things go wrong.
      //
      auto g (
        make_exception_guard (
          [&pkg] ()
          {
            info << "while satisfying " << pkg.available_name_version ();
          }));

      const shared_ptr<available_package>& ap (pkg.available);
      const shared_ptr<repository_fragment>& af (pkg.repository_fragment);
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
          // else
          //
          // @@ TODO: in the future we would need to at least make sure the
          // build and target machines are the same. See also pkg-configure.
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

        auto i (map_.find (dn));
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
                info << name << " depends on (" << dn << " "
                   << *dp.constraint << ")" <<
                info << c.dependent << " depends on (" << dn << " "
                     << c.value << ")" <<
                info << "specify " << dn << " version to satisfy " << name
                     << " constraint";
          }
        }

        const dependency& d (!dep_constr
                             ? dp
                             : dependency {dn, *dep_constr});

        // First see if this package is already selected. If we already have
        // it in the configuraion and it satisfies our dependency version
        // constraint, then we don't want to be forcing its upgrade (or,
        // worse, downgrade).
        //
        shared_ptr<selected_package> dsp (db.find<selected_package> (dn));

        pair<shared_ptr<available_package>,
             shared_ptr<repository_fragment>> rp;

        shared_ptr<available_package>& dap (rp.first);

        bool force (false);

        if (dsp != nullptr)
        {
          if (dsp->state == package_state::broken)
            fail << "unable to build broken package " << dn <<
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
            shared_ptr<repository_fragment> root (
              db.load<repository_fragment> (""));

            rp = system
              ? find_available_one (db, dn, nullopt, root)
              : find_available_one (db,
                                    dn,
                                    version_constraint (dsp->version),
                                    root);

            // A stub satisfies any version constraint so we weed them out
            // (returning stub as an available package feels wrong).
            //
            if (dap == nullptr || dap->stub ())
              rp = make_available (options, cd, db, dsp);
          }
          else
            // Remember that we may be forcing up/downgrade; we will deal with
            // it below.
            //
            force = true;
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
            fail << "package " << pkg.available_name_version ()
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
          rp = find_available_one (db,
                                   dn,
                                   !system ? d.constraint : nullopt,
                                   af);

          if (dap == nullptr)
          {
            if (dep_constr && !system && postponed)
            {
              postponed->insert (&pkg);
              return;
            }

            diag_record dr (fail);
            dr << "unknown dependency " << dn;

            // We need to be careful not to print the wildcard-based
            // constraint.
            //
            if (d.constraint && (!dep_constr || !wildcard (*dep_constr)))
              dr << ' ' << *d.constraint;

            dr << " of package " << name;

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
            if (dap->system_version () == nullptr)
              fail << "dependency " << d << " of package " << name << " is "
                   << "not available in source" <<
                info << "specify ?sys:" << dn << " if it is available from "
                   << "the system";

            if (!satisfies (*dap->system_version (), d.constraint))
              fail << "dependency " << d << " of package " << name << " is "
                   << "not available in source" <<
                info << package_string (dn,
                                        *dap->system_version (),
                                        true /* system */)
                   << " does not satisfy the constrains";

            system = true;
          }
          else
          {
            auto p (dap->system_version_authoritative ());

            if (p.first != nullptr &&
                p.second && // Authoritative.
                satisfies (*p.first, d.constraint))
              system = true;
          }
        }

        build_package bp {
          build_package::build,
          dsp,
          dap,
          rp.second,
          nullopt,    // Hold package.
          nullopt,    // Hold version.
          {},         // Constraints.
          system,
          false,      // Keep output directory.
          strings (), // Configuration variables.
          {name},     // Required by (dependent).
          0};         // Adjustments.

        // Add our constraint, if we have one.
        //
        // Note that we always add the constraint implied by the dependent. The
        // user-implied constraint, if present, will be added when merging from
        // the pre-entered entry. So we will have both constraints for
        // completeness.
        //
        if (dp.constraint)
          bp.constraints.emplace_back (name.string (), *dp.constraint);

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
          collect_build (options, cd, db, move (bp), postponed));

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
              << "package " << name << " dependency on "
              << (c ? "(" : "") << d << (c ? ")" : "") << " is forcing "
              << (u ? "up" : "down") << "grade of " << *dsp << " to ";

            // Print both (old and new) package names in full if the system
            // attribution changes.
            //
            if (dsp->system ())
              dr << p->available_name_version ();
            else
              dr << av; // Can't be a system version so is never wildcard.

            if (dsp->hold_version)
              dr << info << "package version " << *dsp << " is held";

            if (f)
              dr << info << "explicitly request version "
                 << (u ? "up" : "down") << "grade to continue";
          }
        }
      }
    }

    // Collect the package being dropped.
    //
    void
    collect_drop (shared_ptr<selected_package> sp)
    {
      const package_name& nm (sp->name);

      build_package p {
        build_package::drop,
        move (sp),
        nullptr,
        nullptr,
        nullopt,    // Hold package.
        nullopt,    // Hold version.
        {},         // Constraints.
        false,      // System package.
        false,      // Keep output directory.
        strings (), // Configuration variables.
        {},         // Required by.
        0};         // Adjustments.

      auto i (map_.find (nm));

      if (i != map_.end ())
      {
        build_package& bp (i->second.package);

        // Can't think of the scenario when this happens. We would start
        // collecting from scratch (see below).
        //
        assert (!bp.action || *bp.action != build_package::build);

        // Overwrite the existing (possibly pre-entered or adjustment) entry.
        //
        bp = move (p);
      }
      else
        map_.emplace (nm, data_type {end (), move (p)});
    }

    // Collect the package being unheld.
    //
    void
    collect_unhold (const shared_ptr<selected_package>& sp)
    {
      auto i (map_.find (sp->name));

      // Currently, it must always be pre-entered.
      //
      assert (i != map_.end ());

      build_package& bp (i->second.package);

      if (!bp.action) // Pre-entered.
      {
        build_package p {
          build_package::adjust,
          sp,
          nullptr,
          nullptr,
          nullopt,    // Hold package.
          nullopt,    // Hold version.
          {},         // Constraints.
          false,      // System package.
          false,      // Keep output directory.
          strings (), // Configuration variables.
          {},         // Required by.
          build_package::adjust_unhold};

        p.merge (move (bp));
        bp = move (p);
      }
      else
        bp.adjustments |= build_package::adjust_unhold;
    }

    void
    collect_build_prerequisites (const common_options& o,
                                 const dir_path& cd,
                                 database& db,
                                 const package_name& name,
                                 postponed_packages& postponed)
    {
      auto mi (map_.find (name));
      assert (mi != map_.end ());
      collect_build_prerequisites (o, cd, db, mi->second.package, &postponed);
    }

    void
    collect_build_postponed (const common_options& o,
                             const dir_path& cd,
                             database& db,
                             postponed_packages& pkgs)
    {
      // Try collecting postponed packages for as long as we are making
      // progress.
      //
      for (bool prog (true); !pkgs.empty (); )
      {
        postponed_packages npkgs;

        for (const build_package* p: pkgs)
          collect_build_prerequisites (o, cd, db, *p, prog ? &npkgs : nullptr);

        assert (prog); // collect_build_prerequisites() should have failed.
        prog = (npkgs != pkgs);
        pkgs.swap (npkgs);
      }
    }

    // Order the previously-collected package with the specified name
    // returning its positions. Recursively order the package dependencies
    // being ordered failing if a dependency cycle is detected. If reorder is
    // true, then reorder this package to be considered as "early" as
    // possible.
    //
    iterator
    order (const package_name& name, bool reorder = true)
    {
      package_names chain;
      return order (name, chain, reorder);
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
    collect_order_dependents (database& db)
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
          collect_order_dependents (db, i);
      }
    }

    void
    collect_order_dependents (database& db, iterator pos)
    {
      tracer trace ("collect_order_dependents");

      assert (pos != end ());

      build_package& p (*pos);
      const shared_ptr<selected_package>& sp (p.selected);

      const package_name& n (sp->name);

      // See if we are up/downgrading this package. In particular, the
      // available package could be NULL meaning we are just adjusting.
      //
      int ud (p.available != nullptr
              ? sp->version.compare (p.available_version ())
              : 0);

      using query = query<package_dependent>;

      for (auto& pd: db.query<package_dependent> (query::name == n))
      {
        package_name& dn (pd.name);
        auto i (map_.find (dn));

        // First make sure the up/downgraded package still satisfies this
        // dependent.
        //
        bool check (ud != 0 && pd.constraint);

        // There is one tricky aspect: the dependent could be in the process
        // of being up/downgraded as well. In this case all we need to do is
        // detect this situation and skip the test since all the (new)
        // contraints of this package have been satisfied in collect_build().
        //
        if (check && i != map_.end () && i->second.position != end ())
        {
          build_package& dp (i->second.package);

          check = dp.available == nullptr ||
            (dp.selected->system () == dp.system &&
             dp.selected->version == dp.available_version ());
        }

        if (check)
        {
          const version& av (p.available_version ());
          const version_constraint& c (*pd.constraint);

          if (!satisfies (av, c))
          {
            diag_record dr (fail);

            dr << "unable to " << (ud < 0 ? "up" : "down") << "grade "
               << "package " << *sp << " to ";

            // Print both (old and new) package names in full if the system
            // attribution changes.
            //
            if (p.system != sp->system ())
              dr << p.available_name_version ();
            else
              dr << av; // Can't be the wildcard otherwise would satisfy.

            dr << info << "because package " << dn << " depends on (" << n
               << " " << c << ")";

            string rb;
            if (!p.user_selection ())
            {
              for (const package_name& n: p.required_by)
                rb += ' ' + n.string ();
            }

            if (!rb.empty ())
              dr << info << "package " << p.available_name_version ()
                 << " required by" << rb;

            dr << info << "explicitly request up/downgrade of package " << dn;

            dr << info << "or explicitly specify package " << n << " version "
               << "to manually satisfy these constraints";
          }

          // Add this contraint to the list for completeness.
          //
          p.constraints.emplace_back (dn.string (), c);
        }

        auto adjustment = [&dn, &n, &db] () -> build_package
        {
          shared_ptr<selected_package> dsp (db.load<selected_package> (dn));
          bool system (dsp->system ()); // Save flag before the move(dsp) call.

          return build_package {
            build_package::adjust,
            move (dsp),
            nullptr,     // No available package/repository fragment.
            nullptr,
            nullopt,     // Hold package.
            nullopt,     // Hold version.
            {},          // Constraints.
            system,
            false,       // Keep output directory.
            strings (),  // Configuration variables.
            {n},         // Required by (dependency).
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

          if (!dp.action ||                          // Pre-entered.
              *dp.action != build_package::build ||  // Non-build.
              dpos == end ())                        // Build not in the list.
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
            dp.adjustments |= build_package::adjust_reconfigure;

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
          i = map_.emplace (
            move (dn), data_type {end (), adjustment ()}).first;

          i->second.position = insert (pos, i->second.package);
        }

        // Recursively collect our own dependents inserting them before us.
        //
        // Note that we cannot end up with an infinite recursion for
        // configured packages due to a dependency cycle (see order() for
        // details).
        //
        collect_order_dependents (db, i->second.position);
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
    using package_names = small_vector<reference_wrapper<const package_name>,
                                       16>;

    iterator
    order (const package_name& name, package_names& chain, bool reorder)
    {
      // Every package that we order should have already been collected.
      //
      auto mi (map_.find (name));
      assert (mi != map_.end ());

      build_package& p (mi->second.package);

      assert (p.action); // Can't order just a pre-entered package.

      // Make sure there is no dependency cycle.
      //
      {
        auto i (find (chain.begin (), chain.end (), name));

        if (i != chain.end ())
        {
          diag_record dr (fail);
          dr << "dependency cycle detected involving package " << name;

          auto nv = [this] (const package_name& name)
          {
            auto mi (map_.find (name));
            assert (mi != map_.end ());

            build_package& p (mi->second.package);

            assert (p.action); // See above.

            // We cannot end up with a dependency cycle for actions other than
            // build since these packages are configured and we would fail on
            // a previous run while building them.
            //
            assert (p.available != nullptr);

            return p.available_name_version ();
          };

          for (chain.push_back (name); i != chain.end () - 1; ++i)
            dr << info << nv (*i) << " depends on " << nv (*(i + 1));
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
      const shared_ptr<selected_package>& sp (p.selected);
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
      bool src_conf (sp != nullptr &&
                     sp->state == package_state::configured &&
                     sp->substate != package_substate::system);

      auto disfigure = [] (const build_package& p)
      {
        return p.action && (*p.action == build_package::drop ||
                            p.reconfigure ());
      };

      bool order_disfigured (src_conf && disfigure (p));

      chain.push_back (name);

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
            const package_name& name (p.first.object_id ());

            // The prerequisites may not necessarily be in the map.
            //
            auto i (map_.find (name));
            if (i != map_.end () && i->second.package.action)
              update (order (name, chain, false /* reorder */));
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

            update (order (d.name, chain, false /* reorder */));
          }
        }
      }

      // Order the dependencies being disfigured.
      //
      if (order_disfigured)
      {
        for (const auto& p: sp->prerequisites)
        {
          const package_name& name (p.first.object_id ());

          // The prerequisites may not necessarily be in the map.
          //
          auto i (map_.find (name));

          if (i != map_.end () && disfigure (i->second.package))
            update (order (name, chain, false /* reorder */));
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

    map<package_name, data_type> map_;
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
  struct dependency_package
  {
    package_name name;
    optional<version_constraint> constraint; // nullopt if unspecified.
    shared_ptr<selected_package> selected;   // NULL if not present.
    bool system;
    bool patch;                              // Only for an empty version.
    bool keep_out;
    strings config_vars;                     // Only if not system.
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
    shared_ptr<available_package> available;
    shared_ptr<bpkg::repository_fragment> repository_fragment;
    bool unused;
    bool system; // Is meaningless if unused.
  };

  using package_dependents = vector<pair<shared_ptr<selected_package>,
                                         optional<version_constraint>>>;

  static optional<evaluate_result>
  evaluate_dependency (database&,
                       const shared_ptr<selected_package>&,
                       const optional<version_constraint>& desired,
                       bool desired_sys,
                       bool patch,
                       bool explicitly,
                       const set<shared_ptr<repository_fragment>>&,
                       const package_dependents&,
                       bool ignore_unsatisfiable);

  static optional<evaluate_result>
  evaluate_dependency (database& db,
                       const dependency_packages& deps,
                       const shared_ptr<selected_package>& sp,
                       bool ignore_unsatisfiable)
  {
    tracer trace ("evaluate_dependency");

    assert (sp != nullptr && !sp->hold_package);

    const package_name& nm (sp->name);

    // Query the dependents and bail out if the dependency is unused.
    //
    auto pds (db.query<package_dependent> (
                query<package_dependent>::name == nm));

    if (pds.empty ())
    {
      l5 ([&]{trace << *sp << ": unused";});

      return evaluate_result {nullptr /* available */,
                              nullptr /* repository_fragment */,
                              true    /* unused */,
                              false   /* system */};
    }

    // If there are no user expectations regarding this dependency, then we
    // give no up/down-grade recommendation.
    //
    auto i (find_if (
              deps.begin (), deps.end (),
              [&nm] (const dependency_package& i) {return i.name == nm;}));

    if (i == deps.end ())
      return nullopt;

    // If the selected package matches the user expectations then no package
    // change is required.
    //
    const version& sv (sp->version);
    bool ssys (sp->system ());

    // The requested dependency version constraint and system flag.
    //
    const optional<version_constraint>& dvc (i->constraint); // May be nullopt.
    bool dsys (i->system);

    if (ssys == dsys &&
        dvc          &&
        (ssys ? sv == *dvc->min_version : satisfies (sv, dvc)))
    {
      l5 ([&]{trace << *sp << ": unchanged";});

      return evaluate_result {nullptr /* available */,
                              nullptr /* repository_fragment */,
                              false   /* unused */,
                              false   /* system */};
    }

    // Build a set of repository fragments the dependent packages now come
    // from. Also cache the dependents and the constraints they apply to this
    // dependency.
    //
    set<shared_ptr<repository_fragment>> repo_frags;
    package_dependents dependents;

    for (auto& pd: pds)
    {
      shared_ptr<selected_package> dsp (db.load<selected_package> (pd.name));

      shared_ptr<available_package> dap (
        db.find<available_package> (
          available_package_id (dsp->name, dsp->version)));

      if (dap != nullptr)
      {
        assert (!dap->locations.empty ());

        for (const auto& pl: dap->locations)
          repo_frags.insert (pl.repository_fragment.load ());
      }

      dependents.emplace_back (move (dsp), move (pd.constraint));
    }

    return evaluate_dependency (db,
                                sp,
                                dvc,
                                dsys,
                                i->patch,
                                true /* explicitly */,
                                repo_frags,
                                dependents,
                                ignore_unsatisfiable);
  }

  static optional<evaluate_result>
  evaluate_dependency (database& db,
                       const shared_ptr<selected_package>& sp,
                       const optional<version_constraint>& dvc,
                       bool dsys,
                       bool patch,
                       bool explicitly,
                       const set<shared_ptr<repository_fragment>>& rfs,
                       const package_dependents& dependents,
                       bool ignore_unsatisfiable)
  {
    tracer trace ("evaluate_dependency");

    const package_name& nm (sp->name);
    const version&      sv (sp->version);

    auto no_change = [] ()
    {
      return evaluate_result {nullptr /* available */,
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
          l5 ([&]{trace << *sp << ": non-patchable";});
          return no_change ();
        }
      }
    }
    else if (!dsys)
      c = dvc;

    vector<pair<shared_ptr<available_package>,
                shared_ptr<repository_fragment>>> afs (
      find_available (db,
                      nm,
                      c,
                      vector<shared_ptr<repository_fragment>> (rfs.begin (),
                                                               rfs.end ())));

    // Go through up/down-grade candidates and pick the first one that
    // satisfies all the dependents. Collect (and sort) unsatisfied dependents
    // per the unsatisfiable version in case we need to print them.
    //
    struct compare_sp
    {
      bool
      operator() (const shared_ptr<selected_package>& x,
                  const shared_ptr<selected_package>& y) const
      {
        return x->name < y->name;
      }
    };

    using sp_set = set<reference_wrapper<const shared_ptr<selected_package>>,
                       compare_sp>;

    vector<pair<version, sp_set>> unsatisfiable;

    bool stub (false);
    bool ssys (sp->system ());

    assert (!dsys || system_repository.find (nm) != nullptr);

    for (auto& af: afs)
    {
      shared_ptr<available_package>& ap (af.first);
      const version& av (!dsys ? ap->version : *ap->system_version ());

      // If we aim to upgrade to the latest version and it tends to be less
      // then the selected one, then what we currently have is the best that
      // we can get, and so we return the "no change" result.
      //
      // Note that we also handle a package stub here.
      //
      if (!dvc && av < sv)
      {
        assert (!dsys); // Version can't be empty for the system package.

        // For the selected system package we still need to pick a source
        // package version to downgrade to.
        //
        if (!ssys)
        {
          l5 ([&]{trace << *sp << ": best";});
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
        if (!satisfies (av, dp.second))
        {
          satisfactory = false;

          // Continue to collect dependents of the unsatisfiable version if
          // we need to print them before failing.
          //
          if (ignore_unsatisfiable)
            break;

          unsatisfied_dependents.insert (dp.first);
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
      if (av == sv && ssys == dsys)
      {
        l5 ([&]{trace << *sp << ": unchanged";});
        return no_change ();
      }

      l5 ([&]{trace << *sp << ": update to "
                    << package_string (nm, av, dsys);});

      return evaluate_result {
        move (ap), move (af.second), false /* unused */, dsys};
    }

    // If we aim to upgrade to the latest version, then what we currently have
    // is the only thing that we can get, and so returning the "no change"
    // result, unless we need to upgrade a package configured as system.
    //
    if (!dvc && !ssys)
    {
      assert (!dsys); // Version cannot be empty for the system package.

      l5 ([&]{trace << *sp << ": only";});
      return no_change ();
    }

    // If the version satisfying the desired dependency version constraint is
    // unavailable or unsatisfiable for some dependents then we fail, unless
    // requested not to do so. In the later case we return the "no change"
    // result.
    //
    if (ignore_unsatisfiable)
    {
      l5 ([&]{trace << package_string (nm, dvc, dsys)
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
        assert (ssys); // Otherwise, we would bail out earlier (see above).

        // Patch (as any upgrade) of a system package is always explicit, so
        // we always fail and never treat the package as being up to date.
        //
        assert (explicitly);

        fail << "patch version for " << *sp << " is not available "
             << "from its dependents' repositories";
      }
      else if (!stub)
        fail << package_string (nm, dsys ? nullopt : dvc)
             << " is not available from its dependents' repositories";
      else // The only available package is a stub.
      {
        // Note that we don't advise to "build" the package as a system one as
        // it is already as such (see above).
        //
        assert (!dvc && !dsys && ssys);

        fail << package_string (nm, dvc) << " is not available in source "
             << "from its dependents' repositories";
      }
    }

    // Issue the diagnostics and fail.
    //
    diag_record dr (fail);
    dr << "package " << nm << " doesn't satisfy its dependents";

    // Print the list of unsatisfiable versions together with dependents they
    // don't satisfy: up to three latest versions with no more than five
    // dependents each.
    //
    size_t nv (0);
    for (const auto& u: unsatisfiable)
    {
      dr << info << package_string (nm, u.first) << " doesn't satisfy";

      size_t n (0);
      const sp_set& ps (u.second);
      for (const shared_ptr<selected_package>& p: ps)
      {
        dr << ' ' << *p;

        if (++n == 5 && ps.size () != 6) // Printing 'and 1 more' looks stupid.
          break;
      }

      if (n != ps.size ())
        dr << " and " << ps.size () - n << " more";

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
                        const recursive_packages& recs,
                        bool recursion = false)
  {
    auto i (find_if (recs.begin (), recs.end (),
                     [&nm] (const recursive_package& i) -> bool
                     {
                       return i.name == nm;
                     }));

    optional<bool> r;

    if (i != recs.end () && i->recursive >= recursion)
    {
      r = i->upgrade;

      if (*r) // Upgrade (vs patch)?
        return r;
    }

    for (const auto& pd: db.query<package_dependent> (
           query<package_dependent>::name == nm))
    {
      // Note that we cannot end up with an infinite recursion for configured
      // packages due to a dependency cycle (see order() for details).
      //
      if (optional<bool> u = upgrade_dependencies (db, pd.name, recs, true))
      {
        if (!r || *r < *u) // Upgrade wins patch.
        {
          r = u;

          if (*r) // Upgrade (vs patch)?
            return r;
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
                      const recursive_packages& recs,
                      const shared_ptr<selected_package>& sp,
                      bool ignore_unsatisfiable)
  {
    tracer trace ("evaluate_recursive");

    assert (sp != nullptr);

    // Build a set of repository fragment the dependent packages come from.
    // Also cache the dependents and the constraints they apply to this
    // dependency.
    //
    set<shared_ptr<repository_fragment>> repo_frags;
    package_dependents dependents;

    auto pds (db.query<package_dependent> (
                query<package_dependent>::name == sp->name));

    // Only collect repository fragments (for best version selection) of
    // (immediate) dependents that have a hit (direct or indirect) in recs.
    // Note, however, that we collect constraints from all the dependents.
    //
    optional<bool> upgrade;

    for (const auto& pd: pds)
    {
      shared_ptr<selected_package> dsp (db.load<selected_package> (pd.name));
      dependents.emplace_back (dsp, move (pd.constraint));

      if (optional<bool> u = upgrade_dependencies (db, pd.name, recs))
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
      shared_ptr<available_package> dap (
        db.find<available_package> (
          available_package_id (dsp->name, dsp->version)));

      if (dap != nullptr)
      {
        assert (!dap->locations.empty ());

        for (const auto& pl: dap->locations)
          repo_frags.insert (pl.repository_fragment.load ());
      }
    }

    if (!upgrade)
    {
      l5 ([&]{trace << *sp << ": no hit";});
      return nullopt;
    }

    // Recommends the highest possible version.
    //
    optional<evaluate_result> r (
      evaluate_dependency (db,
                           sp,
                           nullopt /* desired */,
                           false /*desired_sys */,
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

  static void
  execute_plan (const pkg_build_options&,
                const dir_path&,
                database&,
                build_package_list&,
                bool simulate);

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
      fail << "multiple --(upgrade|patch)-(immediate|recursive) specified";

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
  }

  static bool
  compare_options (const pkg_options& x, const pkg_options& y)
  {
    return x.keep_out ()          == y.keep_out ()          &&
           x.dependency ()        == y.dependency ()        &&
           x.upgrade ()           == y.upgrade ()           &&
           x.patch ()             == y.patch ()             &&
           x.immediate ()         == y.immediate ()         &&
           x.recursive ()         == y.recursive ()         &&
           x.upgrade_immediate () == y.upgrade_immediate () &&
           x.upgrade_recursive () == y.upgrade_recursive () &&
           x.patch_immediate ()   == y.patch_immediate ()   &&
           x.patch_recursive ()   == y.patch_recursive ();
  }

  static string
  print_options (const pkg_options& o, bool dep = true)
  {
    string r (dep && o.dependency () ? "--dependency" : string ());

    auto add = [&r] (bool v, const char* o)
    {
      if (v)
      {
        if (!r.empty ())
          r += ' ';
        r += o;
      }
    };

    add (o.keep_out (),          "--keep-out");
    add (o.upgrade (),           "--upgrade");
    add (o.patch (),             "--patch");
    add (o.immediate (),         "--immediate");
    add (o.recursive (),         "--recursive");
    add (o.upgrade_immediate (), "--upgrade-immediate");
    add (o.upgrade_recursive (), "--upgrade-recursive");
    add (o.patch_immediate (),   "--patch-immediate");
    add (o.patch_recursive (),   "--patch-recursive");

    return r;
  }

  int
  pkg_build (const pkg_build_options& o, cli::group_scanner& args)
  {
    tracer trace ("pkg_build");

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    validate_options (o, ""); // Global package options.

    if (o.update_dependent () && o.leave_dependent ())
      fail << "both --update-dependent|-U and --leave-dependent|-L "
           << "specified" <<
        info << "run 'bpkg help pkg-build' for more information";

    if (!args.more () && !o.upgrade () && !o.patch ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-build' for more information";

    database db (open (c, trace)); // Also populates the system repository.

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
    struct pkg_spec
    {
      string packages;
      repository_location location;
      pkg_options options;
      strings config_vars;
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

      vector<repository_location> locations;

      transaction t (db);

      while (args.more ())
      {
        string a (args.next ());

        // Make sure the argument can not be misinterpreted as a configuration
        // variable.
        //
        if (a.find ('=') != string::npos && !sep)
          fail << "unexpected configuration variable '" << a << "'" <<
            info << "use the '--' separator to treat it as a package";

        specs.emplace_back ();
        pkg_spec& ps (specs.back ());

        try
        {
          auto& po (ps.options);

          cli::scanner& ag (args.group ());
          po.parse (ag, cli::unknown_mode::fail, cli::unknown_mode::stop);

          // Merge the common and package-specific configuration variables
          // (commons go first).
          //
          ps.config_vars = cvars;

          while (ag.more ())
          {
            string a (ag.next ());
            if (a.find ('=') == string::npos)
              fail << "unexpected group argument '" << a << "'";

            ps.config_vars.push_back (move (a));
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
          fail << e << " grouped for argument '" << a << "'";
        }

        if (!a.empty () && a[0] == '?')
        {
          ps.options.dependency (true);
          a.erase (0, 1);
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

          // Search for the repository location in the database before trying
          // to parse it. Note that the straight parsing could otherwise fail,
          // being unable to properly guess the repository type.
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

            auto q (db.query<repository> (query::location.url == l));
            auto i (q.begin ());

            if (i != q.end ())
            {
              r = i.load ();

              // Fallback to parsing the location if several repositories
              // match.
              //
              if (++i != q.end ())
                r = nullptr;
            }
          }

          ps.location = r != nullptr
            ? r->location
            : parse_location (l, nullopt /* type */);

          if (p > 1)
            ps.packages = string (a, 0, p - 1);

          if (!o.no_fetch ())
          {
            auto pr = [&ps] (const repository_location& i) -> bool
            {
              return i.canonical_name () == ps.location.canonical_name ();
            };

            auto i (find_if (locations.begin (), locations.end (), pr));

            if (i != locations.end ())
              *i = ps.location;
            else
              locations.push_back (ps.location);
          }
        }
        else
          ps.packages = move (a);
      }

      t.commit ();

      if (!locations.empty ())
        rep_fetch (o,
                   c,
                   db,
                   locations,
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
      package_scheme               scheme;
      package_name                 name;
      optional<version_constraint> constraint;
      string                       value;
      pkg_options                  options;
      strings                      config_vars;
    };

    // Create the parsed package argument.
    //
    auto arg_package = [] (package_scheme sc,
                           package_name nm,
                           optional<version_constraint> vc,
                           pkg_options os,
                           strings vs) -> pkg_arg
    {
      assert (!vc || !vc->empty ()); // May not be empty if present.

      pkg_arg r {sc, move (nm), move (vc), string (), move (os), move (vs)};

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

          const system_package* sp (system_repository.find (r.name));

          // Will deal with all the duplicates later.
          //
          if (sp == nullptr || !sp->authoritative)
            system_repository.insert (r.name,
                                      *r.constraint->min_version,
                                      true /* authoritative */);

          break;
        }
      case package_scheme::none: break; // Nothing to do.
      }

      return r;
    };

    // Create the unparsed package argument.
    //
    auto arg_raw = [] (string v, pkg_options os, strings vs) -> pkg_arg
    {
      return pkg_arg {package_scheme::none,
                      package_name (),
                      nullopt /* constraint */,
                      move (v),
                      move (os),
                      move (vs)};
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
      auto append = [&r] (const string& a)
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
        append (a.value);

      if (options)
      {
        string s (print_options (a.options, false));

        if (!s.empty () || !a.config_vars.empty ())
        {
          r += " +{ ";

          if (!s.empty ())
            r += s + ' ';

          for (const string& v: a.config_vars)
          {
            append (v);
            r += ' ';
          }

          r += '}';
        }
      }

      return r;
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

      transaction t (db);

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

            pkg_args.push_back (arg_package (sc,
                                             move (n),
                                             move (vc),
                                             move (ps.options),
                                             move (ps.config_vars)));
          }
          else                           // Add unparsed.
            pkg_args.push_back (arg_raw (move (ps.packages),
                                         move (ps.options),
                                         move (ps.config_vars)));

          continue;
        }

        // Expand the [[<packages>]@]<location> spec. Fail if the repository
        // is not found in this configuration, that can be the case in the
        // presence of --no-fetch option.
        //
        shared_ptr<repository> r (
          db.find<repository> (ps.location.canonical_name ()));

        if (r == nullptr)
          fail << "repository '" << ps.location
               << "' does not exist in this configuration";

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

            for (const auto& rp: db.query<repository_fragment_package> (
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
                  db.find<selected_package> (nm));

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
              pkg_args.push_back (arg_package (package_scheme::none,
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
                    (sp = db.find<selected_package> (n)) != nullptr)
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
              find_available_one (db, n, c, rfs, false /* prereq */).first);

            // Fail if no available package is found or only a stub is
            // available and we are building a source package.
            //
            if (ap == nullptr || (ap->stub () && !sys))
            {
              diag_record dr (fail);

              // If the selected package is loaded then we aim to patch it.
              //
              if (sp != nullptr)
                dr << "patch version for " << *sp << " is not found in "
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
            pkg_args.push_back (arg_package (sc,
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
      map<package_name, pkg_arg> package_map;

      auto check_dup = [&package_map, &arg_string, arg_parsed] (
        const pkg_arg& pa) -> bool
      {
        assert (arg_parsed (pa));

        auto r (package_map.emplace (pa.name, pa));

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
             a.constraint != pa.constraint            ||
             !compare_options (a.options, pa.options) ||
             a.config_vars != pa.config_vars))
          fail << "duplicate package " << pa.name <<
            info << "first mentioned as " << arg_string (r.first->second) <<
            info << "second mentioned as " << arg_string (pa);

        return !r.second;
      };

      transaction t (db);

      shared_ptr<repository_fragment> root (db.load<repository_fragment> (""));

      // Here is what happens here: for unparsed package args we are going to
      // try and guess whether we are dealing with a package archive, package
      // directory, or package name/version by first trying it as an archive,
      // then as a directory, and then assume it is name/version. Sometimes,
      // however, it is really one of the first two but just broken. In this
      // case things are really confusing since we suppress all diagnostics
      // for the first two "guesses". So what we are going to do here is re-run
      // them with full diagnostics if the name/version guess doesn't pan out.
      //
      bool diag (false);
      for (auto i (pkg_args.begin ()); i != pkg_args.end (); )
      {
        pkg_arg& pa (*i);

        // Reduce all the potential variations (archive, directory, package
        // name, package name/version) to a single available_package object.
        //
        shared_ptr<repository_fragment> af;
        shared_ptr<available_package> ap;

        if (!arg_parsed (pa))
        {
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

              pa = arg_package (package_scheme::none,
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
          catch (const failed&)
          {
            // If this is a valid package archive but something went wrong
            // afterwards, then we are done.
            //
            if (package_arc)
              throw;
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

                package_manifest m (
                  pkg_verify (
                    d,
                    true /* ignore_unknown */,
                    [&o, &d] (version& v)
                    {
                      if (optional<version> pv = package_version (o, d))
                        v = move (*pv);
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
                                       c,
                                       t,
                                       d,
                                       m.name,
                                       m.version,
                                       true /* check_external */))
                  m.version = move (*v);

                pa = arg_package (package_scheme::none,
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
            catch (const failed&)
            {
              // If this is a valid package directory but something went wrong
              // afterwards, then we are done.
              //
              if (package_dir)
                throw;
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

              pa = arg_package (package_scheme::none,
                                move (n),
                                move (vc),
                                move (pa.options),
                                move (pa.config_vars));
            }

            l4 ([&]{trace << "package: " << arg_string (pa);});

            if (!pa.options.dependency ())
            {
              // Either get the user-specified version or the latest allowed
              // for a source code package. For a system package we pick the
              // latest one just to make sure the package is recognized.
              //
              optional<version_constraint> c;

              if (!pa.constraint)
              {
                assert (!arg_sys (pa));

                if (pa.options.patch () &&
                    (sp = db.find<selected_package> (pa.name)) != nullptr)
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

              auto rp (find_available_one (db, pa.name, c, root));
              ap = move (rp.first);
              af = move (rp.second);
            }
          }
          catch (const failed&)
          {
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

            rec_pkgs.push_back (recursive_package {pa.name, *u, *r});
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
          auto apr (!pa.constraint || sys
                    ? find_available (db, pa.name, nullopt)
                    : find_available (db, pa.name, *pa.constraint));

          if (apr.empty ())
          {
            diag_record dr (fail);

            dr << "unknown package " << arg_string (pa, false /* options */);
            check_any_available (c, t, &dr);
          }

          // Save before the name move.
          //
          sp = db.find<selected_package> (pa.name);

          dep_pkgs.push_back (dependency_package {move (pa.name),
                                                  move (pa.constraint),
                                                  move (sp),
                                                  sys,
                                                  pa.options.patch (),
                                                  pa.options.keep_out (),
                                                  move (pa.config_vars)});
          continue;
        }

        // Add the held package to the list.
        //
        // Load the package that may have already been selected (if not done
        // yet) and figure out what exactly we need to do here. The end goal
        // is the available_package object corresponding to the actual
        // package that we will be building (which may or may not be
        // the same as the selected package).
        //
        if (sp == nullptr)
          sp = db.find<selected_package> (pa.name);

        if (sp != nullptr && sp->state == package_state::broken)
          fail << "unable to build broken package " << pa.name <<
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
                find_available_one (db,
                                    pa.name,
                                    nullopt,
                                    root).first != nullptr)
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
            check_any_available (c, t, &dr);
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

          auto rp (make_available (o, c, db, sp));
          ap = rp.first;
          af = rp.second; // Could be NULL (orphan).
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
        build_package p {
          build_package::build,
          move (sp),
          move (ap),
          move (af),
          true,                       // Hold package.
          pa.constraint.has_value (), // Hold version.
          {},                         // Constraints.
          arg_sys (pa),
          keep_out,
          move (pa.config_vars),
          {package_name ()},          // Required by (command line).
          0};                         // Adjustments.

        l4 ([&]{trace << "stashing held package "
                      << p.available_name_version ();});

        // "Fix" the version the user asked for by adding the constraint.
        //
        // Note: for a system package this must always be present (so that
        // this build_package instance is never replaced).
        //
        if (pa.constraint)
          p.constraints.emplace_back ("command line", move (*pa.constraint));

        hold_pkgs.push_back (move (p));
      }

      // If this is just pkg-build -u|-p, then we are upgrading all held
      // packages.
      //
      if (hold_pkgs.empty () && dep_pkgs.empty () &&
          (o.upgrade () || o.patch ()))
      {
        using query = query<selected_package>;

        for (shared_ptr<selected_package> sp:
               pointer_result (
                 db.query<selected_package> (query::state == "configured" &&
                                             query::hold_package)))
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

            // Skip the non-patchable selected package. Note that the warning
            // have already been issued in this case.
            //
            if (!pc)
              continue;
          }

          auto apr (find_available_one (db, name, pc, root));

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
            check_any_available (c, t, &dr);
          }

          // We will keep the output directory only if the external package is
          // replaced with an external one (see above for details).
          //
          bool keep_out (o.keep_out () && sp->external ());

          build_package p {
            build_package::build,
              move (sp),
              move (ap),
              move (apr.second),
              true,               // Hold package.
              false,              // Hold version.
              {},                 // Constraints.
              false,              // System package.
              keep_out,
              strings (),         // Configuration variables.
              {package_name ()},  // Required by (command line).
              0};                 // Adjustments.

          l4 ([&]{trace << "stashing held package "
                        << p.available_name_version ();});

          hold_pkgs.push_back (move (p));

          // If there are also -i|-r, then we are also upgrading dependencies
          // of all held packages.
          //
          if (o.immediate () || o.recursive ())
            rec_pkgs.push_back (
              recursive_package {name, o.upgrade (), o.recursive ()});
        }
      }

      t.commit ();
    }

    if (hold_pkgs.empty () && dep_pkgs.empty ())
    {
      assert (rec_pkgs.empty ());

      info << "nothing to build";
      return 0;
    }

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
    // Of course, adding dependecy up/down-grades to the plan can change the
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
        package_name name; // Empty if up/down-grade.

        // Both are NULL if drop.
        //
        shared_ptr<available_package> available;
        shared_ptr<bpkg::repository_fragment> repository_fragment;

        bool system;
      };
      vector<dep> deps;

      // Iteratively refine the plan with dependency up/down-grades/drops.
      //
      for (bool refine (true), scratch (true); refine; )
      {
        transaction t (db);

        build_packages::postponed_packages postponed;

        if (scratch)
        {
          pkgs.clear ();
          postponed.clear ();

          // Pre-enter dependencies to keep track of the desired versions and
          // options specified on the command line. In particular, if the
          // version is specified and the dependency is used as part of the
          // plan, then the desired version must be used. We also need it to
          // distinguish user-driven dependency up/down-grades from the
          // dependent-driven ones, not to warn/refuse.
          //
          // Also, if a dependency package already has selected package that
          // is held, then we need to unhold it.
          //
          for (const dependency_package& p: dep_pkgs)
          {
            build_package bp {
              nullopt,                   // Action.
              nullptr,                   // Selected package.
              nullptr,                   // Available package/repository frag.
              nullptr,
              false,                     // Hold package.
              p.constraint.has_value (), // Hold version.
              {},                        // Constraints.
              p.system,
              p.keep_out,
              p.config_vars,
              {package_name ()},         // Required by (command line).
              0};                        // Adjustments.

            if (p.constraint)
              bp.constraints.emplace_back ("command line", *p.constraint);

            pkgs.enter (p.name, move (bp));
          }

          // Pre-collect user selection to make sure dependency-forced
          // up/down-grades are handled properly (i.e., the order in which we
          // specify packages on the command line does not matter).
          //
          for (const build_package& p: hold_pkgs)
            pkgs.collect_build (o, c, db, p);

          // Collect all the prerequisites of the user selection.
          //
          for (const build_package& p: hold_pkgs)
            pkgs.collect_build_prerequisites (o, c, db, p.name (), postponed);

          // Note that we need to collect unheld after prerequisites, not to
          // overwrite the pre-entered entries before they are used to provide
          // additional constraints for the collected prerequisites.
          //
          for (const dependency_package& p: dep_pkgs)
          {
            if (p.selected != nullptr && p.selected->hold_package)
              pkgs.collect_unhold (p.selected);
          }

          scratch = false;
        }
        else
          pkgs.clear_order (); // Only clear the ordered list.

        // Add to the plan dependencies to up/down-grade/drop that were
        // discovered on the previous iterations.
        //
        for (const dep& d: deps)
        {
          if (d.available == nullptr)
            pkgs.collect_drop (db.load<selected_package> (d.name));
          else
          {
            shared_ptr<selected_package> sp (
              db.find<selected_package> (d.name));

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
              move (sp),
              d.available,
              d.repository_fragment,
              nullopt,               // Hold package.
              nullopt,               // Hold version.
              {},                    // Constraints.
              d.system,
              keep_out,
              strings (),            // Configuration variables.
              {package_name ()},     // Required by (command line).
              0};                    // Adjustments.

            pkgs.collect_build (o, c, db, p, &postponed /* recursively */);
          }
        }

        // Handle the (combined) postponed collection.
        //
        if (!postponed.empty ())
          pkgs.collect_build_postponed (o, c, db, postponed);

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
          pkgs.order (d.name, false /* reorder */);

        for (const build_package& p: reverse_iterate (hold_pkgs))
          pkgs.order (p.name ());

        // Collect and order all the dependents that we will need to
        // reconfigure because of the up/down-grades of packages that are now
        // on the list.
        //
        pkgs.collect_order_dependents (db);

        // And, finally, make sure all the packages that we need to unhold
        // are on the list.
        //
        for (const dependency_package& p: dep_pkgs)
        {
          if (p.selected != nullptr && p.selected->hold_package)
            pkgs.order (p.name, false /* reorder */);
        }

        // We are about to execute the plan on the database (but not on the
        // filesystem / actual packages). Save the session state for the
        // selected_package objects so that we can restore it later (see
        // below for details).
        //
        using selected_packages = session::object_map<selected_package>;
        auto selected_packages_session = [&db, &ses] () -> selected_packages*
        {
          auto& m (ses.map ()[&db]);
          auto i = m.find (&typeid (selected_package));
          return (i != m.end ()
                  ? &static_cast<selected_packages&> (*i->second)
                  : nullptr);
        };

        selected_packages old_sp;
        if (const selected_packages* sp = selected_packages_session ())
          old_sp = *sp;

        // Note that we need to perform the execution on the copies of the
        // build/drop_package objects to preserve the original ones. The
        // selected_package objects will still be changed so we will reload
        // them afterwards (see below).
        //
        {
          vector<build_package> tmp (pkgs.begin (), pkgs.end ());
          build_package_list bl (tmp.begin (), tmp.end ());

          execute_plan (o, c, db, bl, true /* simulate */);
        }

        // Return nullopt if no changes to the dependency are necessary. This
        // value covers both the "no change is required" and the "no
        // recommendation available" cases.
        //
        auto eval_dep = [&db, &dep_pkgs, &rec_pkgs]  (
          const shared_ptr<selected_package>& sp,
          bool ignore_unsatisfiable = true) -> optional<evaluate_result>
        {
          optional<evaluate_result> r;

          // See if there is an optional dependency upgrade recommendation.
          //
          if (!sp->hold_package)
            r = evaluate_dependency (db, dep_pkgs, sp, ignore_unsatisfiable);

          // If none, then see for the recursive dependency upgrade
          // recommendation.
          //
          // Let's skip upgrading system packages as they are, probably,
          // configured as such for a reason.
          //
          if (!r && !sp->system () && !rec_pkgs.empty ())
            r = evaluate_recursive (db, rec_pkgs, sp, ignore_unsatisfiable);

          // Translate the "no change" result to nullopt.
          //
          return r && r->available == nullptr && !r->unused ? nullopt : r;
        };

        // The empty version means that the package must be dropped.
        //
        const version ev;
        auto target_version = [&ev] (const shared_ptr<available_package>& ap,
                                     bool sys) -> const version&
        {
          if (ap == nullptr)
            return ev;

          if (sys)
          {
            assert (ap->system_version () != nullptr);
            return *ap->system_version ();
          }

          return ap->version;
        };

        // Verify that none of the previously-made upgrade/downgrade/drop
        // decisions have changed.
        //
        for (auto i (deps.begin ()); i != deps.end (); )
        {
          bool s (false);

          // Here we scratch if evaluate changed its mind or if the resulting
          // version doesn't match what we expect it to be.
          //
          if (auto sp = db.find<selected_package> (i->name))
          {
            const version& dv (target_version (i->available, i->system));

            if (optional<evaluate_result> r = eval_dep (sp))
              s = dv != target_version (r->available, r->system) ||
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

        if (!scratch)
        {
          // First, we check if the refinement is required, ignoring the
          // unsatisfiable dependency version constraints. If we end up
          // refining the execution plan, such dependencies might be dropped,
          // and then there will be nothing to complain about. When no more
          // refinements are necessary we will run the diagnostics check, to
          // make sure that the unsatisfiable dependency, if left, is
          // reported.
          //
          auto need_refinement = [&eval_dep, &deps, rec_pkgs, &db, &o] (
            bool diag = false) -> bool
          {
            // Examine the new dependency set for any up/down-grade/drops.
            //
            bool r (false); // Presumably no more refinments are necessary.

            using query = query<selected_package>;

            query q (query::state == "configured");

            if (rec_pkgs.empty ())
              q = q && !query::hold_package;

            for (shared_ptr<selected_package> sp:
                   pointer_result (db.query<selected_package> (q)))
            {
              if (optional<evaluate_result> er = eval_dep (sp, !diag))
              {
                // Skip unused if we were instructed to keep them.
                //
                if (o.keep_unused () && er->available == nullptr)
                  continue;

                if (!diag)
                  deps.push_back (dep {sp->name,
                                       move (er->available),
                                       move (er->repository_fragment),
                                       er->system});
                r = true;
              }
            }

            return r;
          };

          refine = need_refinement ();

          if (!refine)
            need_refinement (true /* diag */);
        }

        // Rollback the changes to the database and reload the changed
        // selected_package objects.
        //
        t.rollback ();
        {
          transaction t (db);

          // First reload all the selected_package object that could have been
          // modified (conceptually, we should only modify what's on the
          // plan). And in case of drop the object is removed from the session
          // so we need to bring it back.
          //
          // Note: we use the original pkgs list since the executed ones may
          // contain newly created (but now gone) selected_package objects.
          //
          for (build_package& p: pkgs)
          {
            assert (p.action);

            if (*p.action == build_package::drop)
            {
              assert (p.selected != nullptr);

              ses.cache_insert<selected_package> (
                db, p.selected->name, p.selected);
            }

            if (p.selected != nullptr)
              db.reload (*p.selected);
          }

          // Now remove all the newly created selected_package objects from
          // the session. The tricky part is to distinguish newly created ones
          // from newly loaded (and potentially cached).
          //
          if (selected_packages* sp = selected_packages_session ())
          {
            for (bool rescan (true); rescan; )
            {
              rescan = false;

              for (auto i (sp->begin ()); i != sp->end (); )
              {
                bool erased (false);
                auto j (old_sp.find (i->first));

                if (j == old_sp.end ())
                {
                  if (i->second.use_count () == 1)
                  {
                    // This might cause another object's use count to drop.
                    //
                    i = sp->erase (i);
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
    bool need_prompt (false);

    if (o.print_only () || !o.yes () || o.plan_specified ())
    {
      bool first (true); // First entry in the plan.

      for (const build_package& p: reverse_iterate (pkgs))
      {
        const shared_ptr<selected_package>& sp (p.selected);

        string act;

        assert (p.action);

        if (*p.action == build_package::drop)
        {
          act = "drop " + sp->string () + " (unused)";
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
                  (!p.user_selection () || o.configure_only ()))
                continue;

              act = p.system
                ? "reconfigure"
                : (p.reconfigure ()
                   ? (o.configure_only ()
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

            act += ' ' + p.available_name_version ();
            cause = "required by";
          }

          string rb;
          if (!p.user_selection ())
          {
            for (const package_name& n: p.required_by)
              rb += ' ' + n.string ();

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
      }
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
    // fetch/unpack|checkout. Configure will also be combined with the above
    // operations to guarantee that prerequisite packages are configured by
    // the time its dependents need to be checked out (see the pkg_checkout()
    // function implementation for details).
    //
    // We also have the dependent packages that we reconfigure because their
    // prerequsites got upgraded/downgraded and that the user may want to in
    // addition update (that update_dependents flag above).
    //
    execute_plan (o, c, db, pkgs, false /* simulate */);

    if (o.configure_only ())
      return 0;

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

      if (*p.action != build_package::build)
        continue;

      const shared_ptr<selected_package>& sp (p.selected);

      if (!sp->system () && // System package doesn't need update.
          p.user_selection ())
        upkgs.push_back (pkg_command_vars {sp,
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

        if (*p.action == build_package::adjust && p.reconfigure ())
          upkgs.push_back (pkg_command_vars {p.selected,
                                             strings () /* vars */,
                                             false /* cwd */});
      }
    }

    pkg_update (c, o, o.for_ (), strings (), upkgs);

    if (verb && !o.no_result ())
    {
      for (const pkg_command_vars& pv: upkgs)
        text << "updated " << *pv.pkg;
    }

    return 0;
  }

  static void
  execute_plan (const pkg_build_options& o,
                const dir_path& c,
                database& db,
                build_package_list& build_pkgs,
                bool simulate)
  {
    uint16_t verbose (!simulate ? verb : 0);

    // disfigure
    //
    for (build_package& p: build_pkgs)
    {
      // We are only interested in configured packages that are either being
      // up/down-graded, need reconfiguration (e.g., dependents), or dropped.
      //
      assert (p.action);

      if (*p.action != build_package::drop && !p.reconfigure ())
        continue;

      shared_ptr<selected_package>& sp (p.selected);

      // Each package is disfigured in its own transaction, so that we
      // always leave the configuration in a valid state.
      //
      transaction t (db, !simulate /* start */);

      // Reset the flag if the package being unpacked is not an external one.
      //
      if (p.keep_out && !simulate)
      {
        const shared_ptr<available_package>& ap (p.available);
        const package_location& pl (ap->locations[0]);

        if (pl.repository_fragment.object_id () == "") // Special root.
          p.keep_out = !exists (pl.location); // Directory case.
        else
        {
          p.keep_out = false;

          // See if the package comes from the directory-based repository, and
          // so is external.
          //
          // Note that such repository fragments are always preferred over
          // others (see below).
          //
          for (const package_location& l: ap->locations)
          {
            if (l.repository_fragment.load ()->location.directory_based ())
            {
              p.keep_out = true;
              break;
            }
          }
        }
      }

      // Commits the transaction.
      //
      pkg_disfigure (c, o, t, sp, !p.keep_out, simulate);

      assert (sp->state == package_state::unpacked ||
              sp->state == package_state::transient);

      if (verbose && !o.no_result ())
        text << (sp->state == package_state::transient
                 ? "purged "
                 : "disfigured ") << *sp;

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

    // purge, fetch/unpack|checkout, configure
    //
    for (build_package& p: reverse_iterate (build_pkgs))
    {
      assert (p.action);

      shared_ptr<selected_package>& sp (p.selected);
      const shared_ptr<available_package>& ap (p.available);

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
            transaction t (db, !simulate /* start */);
            pkg_purge (c, t, sp, simulate); // Commits the transaction.

            if (verbose && !o.no_result ())
              text << "purged " << *sp;

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
            transaction t (db, !simulate /* start */);
            pkg_purge (c, t, sp, simulate); // Commits the transaction.

            if (verbose && !o.no_result ())
              text << "purged " << *sp;

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
            transaction t (db, !simulate /* start */);

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
                                c,
                                t,
                                ap->id.name,
                                p.available_version (),
                                true /* replace */,
                                simulate);
                break;
              }
            case repository_basis::version_control:
              {
                sp = pkg_checkout (o,
                                   c,
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
                                 c,
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
            transaction t (db, !simulate /* start */);

            sp = pkg_fetch (
              o,
              c,
              t,
              pl.location, // Archive path.
              true,        // Replace
              false,       // Don't purge; commits the transaction.
              simulate);
          }

          if (sp != nullptr) // Actually fetched or checked out something?
          {
            assert (sp->state == package_state::fetched ||
                    sp->state == package_state::unpacked);

            if (verbose && !o.no_result ())
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
                  dr << "fetched " << *sp;
                  break;
                }
              case repository_basis::directory:
                {
                  assert (sp->state == package_state::unpacked);
                  dr << "using " << *sp << " (external)";
                  break;
                }
              case repository_basis::version_control:
                {
                  assert (sp->state == package_state::unpacked);
                  dr << "checked out " << *sp;
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
            transaction t (db, !simulate /* start */);

            // Commits the transaction.
            //
            sp = pkg_unpack (o, c, t, ap->id.name, simulate);

            if (verbose && !o.no_result ())
              text << "unpacked " << *sp;
          }
          else
          {
            const package_location& pl (ap->locations[0]);
            assert (pl.repository_fragment.object_id () == ""); // Special root.

            transaction t (db, !simulate /* start */);
            sp = pkg_unpack (o,
                             c,
                             t,
                             path_cast<dir_path> (pl.location),
                             true,   // Replace.
                             false,  // Don't purge; commits the transaction.
                             simulate);

            if (verbose && !o.no_result ())
              text << "using " << *sp << " (external)";
          }

          assert (sp->state == package_state::unpacked);
        }

        break; // Get out from the breakout loop.
      }

      // We are done for the dropped package.
      //
      if (*p.action == build_package::drop)
        continue;

      // Configure the package.
      //
      // At this stage the package is either selected, in which case it's a
      // source code one, or just available, in which case it is a system
      // one. Note that a system package gets selected as being configured.
      //
      assert (sp != nullptr || p.system);

      // We configure everything that isn't already configured.
      //
      if (sp != nullptr && sp->state == package_state::configured)
        continue;

      transaction t (db, !simulate /* start */);

      // Note that pkg_configure() commits the transaction.
      //
      if (p.system)
        sp = pkg_configure_system (ap->id.name, p.available_version (), t);
      else if (ap != nullptr)
        pkg_configure (c, o, t, sp, ap->dependencies, p.config_vars, simulate);
      else // Dependent.
      {
        // Must be in the unpacked state since it was disfigured on the first
        // pass (see above).
        //
        assert (sp->state == package_state::unpacked);

        package_manifest m (
          pkg_verify (sp->effective_src_root (c),
                      true /* ignore_unknown */,
                      [&sp] (version& v) {v = sp->version;}));

        pkg_configure (c,
                       o,
                       t,
                       sp,
                       convert (move (m.dependencies)),
                       p.config_vars,
                       simulate);
      }

      assert (sp->state == package_state::configured);

      if (verbose && !o.no_result ())
        text << "configured " << *sp;
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

        transaction t (db, !simulate /* start */);
        db.update (sp);
        t.commit ();

        if (verbose > 1)
        {
          if (hp)
            text << "holding package " << sp->name;

          if (hv)
            text << "holding version " << *sp;
        }
      }
    }
  }
}
