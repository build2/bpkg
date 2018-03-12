// file      : bpkg/pkg-build.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-build.hxx>

#include <map>
#include <set>
#include <list>
#include <cstring>    // strlen()
#include <iostream>   // cout
#include <algorithm>  // find(), find_if()

#include <libbutl/url.mxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/satisfaction.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/common-options.hxx>

#include <bpkg/pkg-drop.hxx>
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
  // @@ TODO
  //
  //    - Detect and complain about dependency cycles.
  //    - Configuration vars (both passed and preserved)
  //

  // Try to find a package that optionally satisfies the specified
  // version constraint. Look in the specified repository, its
  // prerequisite repositories, and their complements, recursively
  // (note: recursivity applies to complements, not prerequisites).
  // Return the package and the repository in which it was found or
  // NULL for both if not found. Note that a stub satisfies any
  // constraint.
  //
  static pair<shared_ptr<available_package>, shared_ptr<repository>>
  find_available (database& db,
                  const string& name,
                  const shared_ptr<repository>& r,
                  const optional<dependency_constraint>& c,
                  bool prereq = true)
  {
    using query = query<available_package>;

    query q (query::id.name == name);
    const auto& vm (query::id.version);

    // If there is a constraint, then translate it to the query. Otherwise,
    // get the latest version or stub versions if present.
    //
    if (c)
    {
      // If the revision is not explicitly specified, then compare ignoring the
      // revision. The idea is that when the user runs 'bpkg build libfoo/1'
      // and there is 1+1 available, it should just work. The user shouldn't
      // have to spell the revision explicitly. Similarly, when we have
      // 'depends: libfoo == 1', then it would be strange if 1+1 did not
      // satisfy this constraint. The same for libfoo <= 1 -- 1+1 should
      // satisfy.
      //
      // Note that strictly speaking 0 doesn't mean unspecified. Which means
      // with this implementation there is no way to say "I really mean
      // revision 0" since 1 == 1+0. One can, in the current model, say libfoo
      // == 1+1, though. This is probably ok since one would assume any
      // subsequent revision of a package version are just as (un)satisfactory
      // as the first one.
      //
      query qs (compare_version_eq (vm, wildcard_version, false));

      if (c->min_version &&
          c->max_version &&
          *c->min_version == *c->max_version)
      {
        const version& v (*c->min_version);

        q = q && (compare_version_eq (vm, v, v.revision != 0) || qs);
      }
      else
      {
        query qr (true);

        if (c->min_version)
        {
          const version& v (*c->min_version);

          if (c->min_open)
            qr = compare_version_gt (vm, v, v.revision != 0);
          else
            qr = compare_version_ge (vm, v, v.revision != 0);
        }

        if (c->max_version)
        {
          const version& v (*c->max_version);

          if (c->max_open)
            qr = qr && compare_version_lt (vm, v, v.revision != 0);
          else
            qr = qr && compare_version_le (vm, v, v.revision != 0);
        }

        q = q && (qr || qs);
      }
    }

    q += order_by_version_desc (vm);

    // Filter the result based on the repository to which each version
    // belongs.
    //
    return filter_one (r, db.query<available_package> (q), prereq);
  }

  // Create a transient (or fake, if you prefer) available_package object
  // corresponding to the specified selected object. Note that the package
  // locations list is left empty and that the returned repository could be
  // NULL if the package is an orphan.
  //
  // Note also that in our model we assume that make_available() is only
  // called if there is no real available_package. This makes sure that if
  // the package moves (e.g., from testing to stable), then we will be using
  // stable to resolve its dependencies.
  //
  static pair<shared_ptr<available_package>, shared_ptr<repository>>
  make_available (const common_options& options,
                  const dir_path& cd,
                  database& db,
                  const shared_ptr<selected_package>& sp)
  {
    assert (sp != nullptr && sp->state != package_state::broken);

    if (sp->system ())
      return make_pair (make_shared<available_package> (sp->name, sp->version),
                        nullptr);

    // First see if we can find its repository.
    //
    // Note that this is package's "old" repository and there is no guarantee
    // that its dependencies are still resolvable from it. But this is our
    // best chance (we could go nuclear and point all orphans to the root
    // repository but that feels a bit too drastic at the moment).
    //
    shared_ptr<repository> ar (
      db.find<repository> (
        sp->repository.canonical_name ()));

    // The package is in at least fetched state, which means we should
    // be able to get its manifest.
    //
    const optional<path>& a (sp->archive);
    const optional<dir_path>& d (sp->src_root);

    package_manifest m (
      sp->state == package_state::fetched
      ? pkg_verify (options, a->absolute () ? *a : cd / *a, true)
      : pkg_verify (d->absolute () ? *d : cd / *d, true));

    // Copy the possibly fixed up version from the selected package.
    //
    if (sp->external ())
      m.version = sp->version;

    return make_pair (make_shared<available_package> (move (m)), move (ar));
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
  // dependencies (the collect() function) and ordering of the list
  // (the order() function).
  //
  // During the satisfaction phase, we collect all the packages, their
  // prerequisites (and so on, recursively) in a map trying to satisfy
  // any dependency constraints. Specifically, during this step, we may
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
    shared_ptr<selected_package>  selected;   // NULL if not selected.
    shared_ptr<available_package> available;  // Can be NULL, fake/transient.
    shared_ptr<bpkg::repository>  repository; // Can be NULL (orphan) or root.

    // Hold flags. Note that we only "increase" the hold_package value that is
    // already in the selected package.
    //
    optional<bool> hold_package;
    optional<bool> hold_version;

    // Constraint value plus, normally, the dependent package name that
    // placed this constraint but can also be some other name for the
    // initial selection (e.g., package version specified by the user
    // on the command line).
    //
    struct constraint_type
    {
      string dependent;
      dependency_constraint value;

      constraint_type () = default;
      constraint_type (string d, dependency_constraint v)
          : dependent (move (d)), value (move (v)) {}
    };

    vector<constraint_type> constraints;

    // System package indicator. See also a note in collect()'s constraint
    // merging code.
    //
    bool system;

    // If the flag is set and the external package is being replaced with an
    // external one, then keep its output directory between upgrades and
    // downgrades.
    //
    bool keep_out;

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

    // Set of package names that caused this package to be built. Empty
    // name signifies user selection.
    //
    set<string> required_by;

    // True if we need to reconfigure this package. If available package
    // is NULL, then reconfigure must be true (this is a dependent that
    // needs to be reconfigured because its prerequisite is being up/down-
    // graded or reconfigured). Note that in some cases reconfigure is
    // naturally implied. For example, if an already configured package
    // is being up/down-graded. For such cases we don't guarantee that
    // the reconfigure flag is true. We only make sure to set it for
    // cases that would otherwise miss the need for the reconfiguration.
    // As a result, use the reconfigure() accessor which detects both
    // explicit and implied cases.
    //
    // At first, it may seem that this flag is redundant and having the
    // available package set to NULL is sufficient. But consider the case
    // where the user asked us to build a package that is already in the
    // configured state (so all we have to do is pkg-update). Next, add
    // to this a prerequisite package that is being upgraded. Now our
    // original package has to be reconfigured. But without this flag
    // we won't know (available for our package won't be NULL).
    //
    bool reconfigure_;

    bool
    reconfigure () const
    {
      return selected != nullptr &&
        selected->state == package_state::configured &&
        (reconfigure_ || // Must be checked first, available could be NULL.
         selected->system () != system ||
         selected->version != available_version ());
    }

    bool
    user_selection () const
    {
      return required_by.find ("") != required_by.end ();
    }

    string
    available_name () const
    {
      assert (available != nullptr);

      const version& v (available_version ());
      string vs (v == wildcard_version ? "/*" : "/" + v.string ());

      return system
        ? "sys:" + available->id.name + vs
        : available->id.name + vs;
    }
  };

  struct build_packages: list<reference_wrapper<build_package>>
  {
    // Collect the package. Return its pointer if this package version was, in
    // fact, added to the map and NULL if it was already there or the existing
    // version was preferred. So can be used as bool.
    //
    build_package*
    collect (const common_options& options,
             const dir_path& cd,
             database& db,
             build_package&& pkg,
             bool recursively)
    {
      using std::swap; // ...and not list::swap().

      tracer trace ("collect");

      assert (pkg.available != nullptr); // No dependents allowed here.
      auto i (map_.find (pkg.available->id.name));

      // If we already have an entry for this package name, then we
      // have to pick one over the other.
      //
      if (i != map_.end ())
      {
        const string& n (i->first);

        // At the end we want p1 to point to the object that we keep
        // and p2 to the object whose constraints we should copy.
        //
        build_package* p1 (&i->second.package);
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
          auto test = [] (build_package* pv, build_package* pc)
            -> const constraint_type*
          {
            for (const constraint_type& c: pc->constraints)
              if (!satisfies (pv->available_version (), c.value))
                return &c;

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
              const string& d1 (c1->dependent);
              const string& d2 (c2->dependent);

              fail << "unable to satisfy constraints on package " << n <<
                info << d1 << " depends on (" << n << " " << c1->value << ")" <<
                info << d2 << " depends on (" << n << " " << c2->value << ")" <<
                info << "available " << p1->available_name () <<
                info << "available " << p2->available_name () <<
                info << "explicitly specify " << n << " version to manually "
                   << "satisfy both constraints";
            }
            else
              swap (p1, p2);
          }

          l4 ([&]{trace << "pick " << p1->available_name ()
                        << " over " << p2->available_name ();});
        }
        // If versions are the same, then we still need to pick the entry as
        // one of them can build a package from source while another configure
        // a system package. We prefer a user-selected entry (if there is
        // one). If none of them is user-selected we prefer a source package
        // over a system one. Copy the constraints from the thrown aways entry
        // to the selected one.
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
          swap (p1, p2); // Setup for constraints copying below.
        }

        p1->constraints.insert (p1->constraints.end (),
                                make_move_iterator (p2->constraints.begin ()),
                                make_move_iterator (p2->constraints.end ()));

        p1->required_by.insert (p2->required_by.begin (),
                                p2->required_by.end ());

        // Also copy hold_* flags if they are "stronger".
        //
        if (!p1->hold_package ||
            (p2->hold_package && *p2->hold_package > *p1->hold_package))
          p1->hold_package = p2->hold_package;

        if (!p1->hold_version ||
            (p2->hold_version && *p2->hold_version > *p1->hold_version))
          p1->hold_version = p2->hold_version;

        // Save the 'keep output directory' flag if specified by the user.
        //
        if (p2->user_selection () && p2->keep_out)
          p1->keep_out = true;

        // Note that we don't copy the build_package::system flag. If it was
        // set from the command line ("strong system") then we will also have
        // the '== 0' constraint which means that this build_package object
        // will never be replaced.
        //
        // For other cases ("weak system") we don't want to copy system over
        // in order not prevent, for example, system to non-system upgrade.

        if (!replace)
          return nullptr;
      }
      else
      {
        // This is the first time we are adding this package name to the map.
        //
        l4 ([&]{trace << "add " << pkg.available_name ();});

        string n (pkg.available->id.name); // Note: copy; see emplace() below.
        i = map_.emplace (move (n), data_type {end (), move (pkg)}).first;
      }

      build_package& p (i->second.package);

      if (recursively)
        collect_prerequisites (options, cd, db, p);

      return &p;
    }

    // Collect the package prerequisites recursively. But first "prune" this
    // process if the package we build is a system one or is already configured
    // since that would mean all its prerequisites are configured as well. Note
    // that this is not merely an optimization: the package could be an orphan
    // in which case the below logic will fail (no repository in which to
    // search for prerequisites). By skipping the prerequisite check we are
    // able to gracefully handle configured orphans.
    //
    void
    collect_prerequisites (const common_options& options,
                           const dir_path& cd,
                           database& db,
                           const build_package& pkg)
    {
      tracer trace ("collect_prerequisites");

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
            info << "while satisfying " << pkg.available_name ();
          }));

      const shared_ptr<available_package>& ap (pkg.available);
      const shared_ptr<repository>& ar (pkg.repository);
      const string& name (ap->id.name);

      for (const dependency_alternatives& da: ap->dependencies)
      {
        if (da.conditional) // @@ TODO
          fail << "conditional dependencies are not yet supported";

        if (da.size () != 1) // @@ TODO
          fail << "multiple dependency alternatives not yet supported";

        const dependency& d (da.front ());
        const string& dn (d.name);

        if (da.buildtime)
        {
          // Handle special names.
          //
          if (dn == "build2")
          {
            if (d.constraint)
              satisfy_build2 (options, name, d);

            continue;
          }
          else if (dn == "bpkg")
          {
            if (d.constraint)
              satisfy_bpkg (options, name, d);

            continue;
          }
          // else
          //
          // @@ TODO: in the future we would need to at least make sure the
          // build and target machines are the same. See also pkg-configure.
        }

        // First see if this package is already selected. If we already have
        // it in the configuraion and it satisfies our dependency constraint,
        // then we don't want to be forcing its upgrade (or, worse,
        // downgrade).
        //
        shared_ptr<selected_package> dsp (db.find<selected_package> (dn));

        pair<shared_ptr<available_package>, shared_ptr<repository>> rp;
        shared_ptr<available_package>& dap (rp.first);

        bool force (false);
        bool system (false);

        if (dsp != nullptr)
        {
          if (dsp->state == package_state::broken)
            fail << "unable to build broken package " << dn <<
              info << "use 'pkg-purge --force' to remove";

          if (satisfies (dsp->version, d.constraint))
          {
            // First try to find an available package for this exact version.
            // In particular, this handles the case where a package moves from
            // one repository to another (e.g., from testing to stable).
            //
            shared_ptr<repository> root (db.load<repository> (""));
            rp = find_available (
              db, dn, root, dependency_constraint (dsp->version));

            // A stub satisfies any dependency constraint so we weed them out
            // by comparing versions (returning stub as an available package
            // feels wrong).
            //
            if (dap == nullptr || dap->version != dsp->version)
              rp = make_available (options, cd, db, dsp);

            system = dsp->system ();
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
          // And if we have no repository to look in, then that means the
          // package is an orphan (we delay this check until we actually
          // need the repository to allow orphans without prerequisites).
          //
          if (ar == nullptr)
            fail << "package " << pkg.available_name () << " is orphaned" <<
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
          // Note that this logic (naturally) does not apply if the package is
          // already selected by the user (see above).
          //
          rp = find_available (db, dn, ar, d.constraint);

          if (dap == nullptr)
          {
            diag_record dr (fail);
            dr << "unknown prerequisite " << d << " of package " << name;

            if (!ar->location.empty ())
              dr << info << "repository " << ar->location << " appears to "
                 << "be broken" <<
                info << "or the repository state could be stale" <<
                info << "run 'bpkg rep-fetch' to update";
          }

          // If all that's available is a stub then we need to make sure the
          // package is present in the system repository and it's version
          // satisfies the constraint. If a source package is available but
          // there is an optional system package specified on the command line
          // and it's version satisfies the constraint then the system package
          // should be preferred. To recognize such a case we just need to
          // check if the authoritative system version is set and it satisfies
          // the constraint. If the corresponding system package is
          // non-optional it will be preferred anyway.
          //
          if (dap->stub ())
          {
            if (dap->system_version () == nullptr)
              fail << "prerequisite " << d << " of package " << name << " is "
                   << "not available in source" <<
                info << "specify ?sys:" << dn << " if it is available from "
                   << "the system";

            if (!satisfies (*dap->system_version (), d.constraint))
            {
              fail << "prerequisite " << d << " of package " << name << " is "
                   << "not available in source" <<
                info << "sys:" << dn << "/" << *dap->system_version ()
                   << " does not satisfy the constrains";
            }

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

        build_package dp {
          dsp,
          dap,
          rp.second,
          nullopt,      // Hold package.
          nullopt,      // Hold version.
          {},           // Constraints.
          system,       // System.
          false,        // Keep output directory.
          {name},       // Required by.
          false};       // Reconfigure.

        // Add our constraint, if we have one.
        //
        if (d.constraint)
          dp.constraints.emplace_back (name, *d.constraint);

        // Now collect this prerequisite. If it was actually collected
        // (i.e., it wasn't already there) and we are forcing an upgrade
        // and the version is not held, then warn, unless we are running
        // quiet. Downgrade or upgrade of a held version -- refuse.
        //
        // Note though that while the prerequisite was collected it could have
        // happen because it is an optional system package and so not being
        // pre-collected earlier. Meanwhile the package version was specified
        // explicitly and we shouldn't consider that as a dependency-driven
        // up/down-grade enforcement. To recognize such a case we just need to
        // check for the system flag, so if it is true then the prerequisite
        // is an optional system package. If it were non-optional it wouldn't
        // be being collected now since it must have been pre-collected
        // earlier. And if it were created from the selected package then
        // the force flag wouldn't haven been true.
        //
        // Here is an example of the situation we need to handle properly:
        //
        // repo: foo/2(->bar/2), bar/0+1
        // build sys:bar/1
        // build foo ?sys:bar/2
        //
        const build_package* p (collect (options, cd, db, move (dp), true));
        if (p != nullptr && force && !p->system)
        {
          const version& av (p->available_version ());

          // Fail if downgrade non-system package or held.
          //
          bool u (av > dsp->version);
          bool f (dsp->hold_version || (!u && !dsp->system ()));

          if (verb || f)
          {
            bool c (d.constraint);
            diag_record dr;

            (f ? dr << fail : dr << warn)
              << "package " << name << " dependency on "
              << (c ? "(" : "") << d << (c ? ")" : "") << " is forcing "
              << (u ? "up" : "down") << "grade of " << *dsp << " to ";

            // Print both (old and new) package names in full if the system
            // attribution changes.
            //
            if (dsp->system ())
              dr << p->available_name ();
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

    void
    collect_prerequisites (const common_options& options,
                           const dir_path& cd,
                           database& db,
                           const string& name)
    {
      auto mi (map_.find (name));
      assert (mi != map_.end ());
      collect_prerequisites (options, cd, db, mi->second.package);
    }

    // Order the previously-collected package with the specified name
    // returning its positions. If reorder is true, then reorder this
    // package to be considered as "early" as possible.
    //
    iterator
    order (const string& name, bool reorder = true)
    {
      // Every package that we order should have already been collected.
      //
      auto mi (map_.find (name));
      assert (mi != map_.end ());

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
      build_package& p (mi->second.package);
      const shared_ptr<selected_package>& sp (p.selected);
      const shared_ptr<available_package>& ap (p.available);

      assert (ap != nullptr); // No dependents allowed here.

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

      // Similar to collect(), we can prune if the package is already
      // configured, right? Right for a system ones but not for others.
      // While in collect() we didn't need to add prerequisites of such a
      // package, it doesn't mean that they actually never ended up in the
      // map via another way. For example, some can be a part of the initial
      // selection. And in that case we must order things properly.
      //
      if (!p.system)
      {
        // So here we are going to do things differently depending on
        // whether the package is already configured or not. If it is and
        // not as a system package, then that means we can use its
        // prerequisites list. Otherwise, we use the manifest data.
        //
        if (sp != nullptr &&
            sp->state == package_state::configured &&
            sp->substate != package_substate::system &&
            sp->version == p.available_version ())
        {
          for (const auto& p: sp->prerequisites)
          {
            const string& name (p.first.object_id ());

            // The prerequisites may not necessarily be in the map.
            //
            if (map_.find (name) != map_.end ())
              update (order (name, false));
          }
        }
        else
        {
          // We are iterating in reverse so that when we iterate over
          // the dependency list (also in reverse), prerequisites will
          // be built in the order that is as close to the manifest as
          // possible.
          //
          for (const dependency_alternatives& da:
                 reverse_iterate (ap->dependencies))
          {
            assert (!da.conditional && da.size () == 1); // @@ TODO
            const dependency& d (da.front ());
            const string& dn (d.name);

            // Skip special names.
            //
            if (da.buildtime && (dn == "build2" || dn == "bpkg"))
              continue;

            update (order (d.name, false));
          }
        }
      }

      return pos = insert (i, p);
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
        if (p.reconfigure ())
          collect_order_dependents (db, i);
      }
    }

    void
    collect_order_dependents (database& db, iterator pos)
    {
      tracer trace ("collect_order_dependents");

      build_package& p (*pos);
      const shared_ptr<selected_package>& sp (p.selected);

      const string& n (sp->name);

      // See if we are up/downgrading this package. In particular, the
      // available package could be NULL meaning we are just reconfiguring.
      //
      int ud (p.available != nullptr
              ? sp->version.compare (p.available_version ())
              : 0);

      using query = query<package_dependent>;

      for (auto& pd: db.query<package_dependent> (query::name == n))
      {
        string& dn (pd.name);
        auto i (map_.find (dn));

        // First make sure the up/downgraded package still satisfies this
        // dependent.
        //
        bool check (ud != 0 && pd.constraint);

        // There is one tricky aspect: the dependent could be in the process
        // of being up/downgraded as well. In this case all we need to do is
        // detect this situation and skip the test since all the (new)
        // contraints of this package have been satisfied in collect().
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
          const dependency_constraint& c (*pd.constraint);

          if (!satisfies (av, c))
          {
            diag_record dr (fail);

            dr << "unable to " << (ud < 0 ? "up" : "down") << "grade "
               << "package " << *sp << " to ";

            // Print both (old and new) package names in full if the system
            // attribution changes.
            //
            if (p.system != sp->system ())
              dr << p.available_name ();
            else
              dr << av; // Can't be the wildcard otherwise would satisfy.

            dr << info << "because package " << dn << " depends on (" << n
               << " " << c << ")";

            string rb;
            if (!p.user_selection ())
            {
              for (const string& n: p.required_by)
                rb += ' ' + n;
            }

            if (!rb.empty ())
              dr << info << "package " << p.available_name ()
                 << " required by" << rb;

            dr << info << "explicitly request up/downgrade of package " << dn;

            dr << info << "or explicitly specify package " << n << " version "
               << "to manually satisfy these constraints";
          }

          // Add this contraint to the list for completeness.
          //
          p.constraints.emplace_back (dn, c);
        }

        // We can have three cases here: the package is already on the
        // list, the package is in the map (but not on the list) and it
        // is in neither.
        //
        if (i != map_.end ())
        {
          // Now add to the list.
          //
          build_package& dp (i->second.package);

          p.required_by.insert (dn);

          // Force reconfiguration in both cases.
          //
          dp.reconfigure_ = true;

          if (i->second.position == end ())
          {
            // Clean the build_package object up to make sure we don't
            // inadvertently force up/down-grade.
            //
            dp.available = nullptr;
            dp.repository = nullptr;

            i->second.position = insert (pos, dp);
          }
        }
        else
        {
          shared_ptr<selected_package> dsp (db.load<selected_package> (dn));
          bool system (dsp->system ()); // Save flag before the move(dsp) call.

          i = map_.emplace (
            move (dn),
            data_type
            {
              end (),
              build_package {
                move (dsp),
                nullptr,
                nullptr,
                nullopt,        // Hold package.
                nullopt,        // Hold version.
                {},             // Constraints.
                system,
                false,          // Keep output directory.
                {n},            // Required by.
                true}           // Reconfigure.
            }).first;

          i->second.position = insert (pos, i->second.package);
        }

        // Collect our own dependents inserting them before us.
        //
        collect_order_dependents (db, i->second.position);
      }
    }

  private:
    struct data_type
    {
      iterator position;         // Note: can be end(), see collect().
      build_package package;
    };

    map<string, data_type> map_;
  };

  int
  pkg_build (const pkg_build_options& o, cli::scanner& a)
  {
    tracer trace ("pkg_build");

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    if (o.drop_prerequisite () && o.keep_prerequisite ())
      fail << "both --drop-prerequisite|-D and --keep-prerequisite|-K "
           << "specified" <<
        info << "run 'bpkg help pkg-build' for more information";

    if (o.update_dependent () && o.leave_dependent ())
      fail << "both --update-dependent|-U and --leave-dependent|-L "
           << "specified" <<
        info << "run 'bpkg help pkg-build' for more information";

    if (!a.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-build' for more information";

    // Check if the argument has the [<packages>]@<location> form or looks
    // like a URL. Return the position of <location> if that's the case and
    // string::npos otherwise.
    //
    // Note that we consider '@' to be such a delimiter only if it comes
    // before ":/" (think a URL which could contain its own '@').
    //
    auto find_location = [] (const string& arg) -> size_t
    {
      using url_traits = butl::url::traits;

      size_t p (0);

      // Skip leading ':' that are not part of a URL.
      //
      while ((p = arg.find_first_of ("@:", p)) != string::npos &&
             arg[p] == ':'                                     &&
             url_traits::find (arg, p) == string::npos)
        ++p;

      if (p != string::npos)
      {
        if (arg[p] == ':')
        {
          // The whole thing must be the location.
          //
          p = url_traits::find (arg, p) == 0 ? 0 : string::npos;
        }
        else
          p += 1; // Skip '@'.
      }

      return p;
    };

    database db (open (c, trace)); // Also populates the system repository.

    // Search for the repository location in the database before trying to
    // parse it. Note that the straight parsing could otherwise fail, being
    // unable to properly guess the repository type.
    //
    auto location = [&db] (const string& l) -> repository_location
    {
      using query = query<repository>;

      shared_ptr<repository> r (
        db.query_one<repository> (query::location.url == l));

      if (r != nullptr)
        return r->location;

      return parse_location (l, nullopt /* type */);
    };

    // Note that the session spans all our transactions. The idea here is
    // that selected_package objects in the build_packages list below will
    // be cached in this session. When subsequent transactions modify any
    // of these objects, they will modify the cached instance, which means
    // our list will always "see" their updated state.
    //
    // Also note that rep_fetch() must be called in session.
    //
    session s;

    // Collect repository locations from <packages>@<location> arguments,
    // suppressing duplicates.
    //
    // Note that the last repository location overrides the previous ones with
    // the same canonical name.
    //
    strings args;
    vector<repository_location> locations;
    {
      transaction t (db.begin ());

      while (a.more ())
      {
        string arg (a.next ());
        size_t p (find_location (arg));

        if (p != string::npos)
        {
          repository_location l (location (string (arg, p)));

          auto pr = [&l] (const repository_location& i) -> bool
          {
            return i.canonical_name () == l.canonical_name ();
          };

          auto i (find_if (locations.begin (), locations.end (), pr));

          if (i != locations.end ())
            *i = move (l);
          else
            locations.push_back (move (l));
        }

        args.push_back (move (arg));
      }

      t.commit ();
    }

    if (!locations.empty ())
      rep_fetch (o, c, db, locations, o.fetch_shallow ());

    // Expand <packages>@<location> arguments.
    //
    strings eargs;
    {
      transaction t (db.begin ());

      for (string& arg: args)
      {
        size_t p (find_location (arg));

        if (p == string::npos)
        {
          eargs.push_back (move (arg));
          continue;
        }

        repository_location l (location (string (arg, p)));
        shared_ptr<repository> r (db.load<repository> (l.canonical_name ()));

        // If no packages are specified explicitly (the argument starts with
        // '@' or is a URL) then we select latest versions of all the packages
        // from this repository. Otherwise, we search for the specified
        // packages and versions (if specified) or latest versions (if
        // unspecified) in the repository and its complements (recursively),
        // failing if any of them are not found.
        //
        if (p == 0 || p == 1) // No packages are specified explicitly.
        {
          // Collect the latest package version.
          //
          map<string, version> pvs;

          using query = query<repository_package>;

          for (const auto& rp: db.query<repository_package> (
                 (query::repository::name == r->name) +
                 order_by_version_desc (query::package::id.version)))
          {
            const shared_ptr<available_package>& p (rp);
            pvs.insert (make_pair (p->id.name, p->version));
          }

          // Populate the argument list with the latest package versions.
          //
          for (const auto& pv: pvs)
            eargs.push_back (pv.first + '/' + pv.second.string ());
        }
        else // Packages with optional versions in the coma-separated list.
        {
          string ps (arg, 0, p - 1);
          for (size_t b (0); b != string::npos;)
          {
            // Extract the package.
            //
            p = ps.find (',', b);

            string pkg (ps, b, p != string::npos ? p - b : p);
            const char* s (pkg.c_str ());

            bool  sys (parse_package_scheme (s) == package_scheme::sys);
            string  n (parse_package_name (s));
            version v (parse_package_version (s));

            // Check if the package is present in the repository and its
            // complements, recursively.
            //
            // Note that for the system package we don't care about its exact
            // version available from the repository (which may well be a
            // stub). All we need is to make sure that it is present in the
            // repository.
            //
            optional<dependency_constraint> c (
              v.empty () || sys
              ? nullopt
              : optional<dependency_constraint> (v));

            shared_ptr<available_package> ap (
              find_available (db, n, r, c, false /* prereq */).first);

            if (ap == nullptr)
            {
              diag_record dr (fail);
              dr << "package " << pkg << " is not found in " << r->name;

              if (!r->complements.empty ())
                dr << " or its complements";
            }

            // Add the [scheme:]package/version to the argument list.
            //
            // Note that the system package is added to the argument list as
            // it appears originally (see above).
            //
            eargs.push_back (sys ? pkg : n + '/' + ap->version.string ());

            b = p != string::npos ? p + 1 : p;
          }
        }
      }

      t.commit ();
    }

    map<string, string> package_arg;

    // Check if the package is a duplicate. Return true if it is but harmless.
    //
    auto check_dup = [&package_arg] (const string& n, const string& a) -> bool
    {
      auto r (package_arg.emplace (n, a));

      if (!r.second && r.first->second != a)
        fail << "duplicate package " << n <<
          info << "first mentioned as " << r.first->second <<
          info << "second mentioned as " << a;

      return !r.second;
    };

    // Pre-scan the arguments and sort them out into optional and mandatory.
    //
    strings pargs;
    for (const string& arg: eargs)
    {
      const char* s (arg.c_str ());

      bool opt (s[0] == '?');
      if (opt)
        ++s;
      else
        pargs.emplace_back (s);

      if (parse_package_scheme (s) == package_scheme::sys)
      {
        string n (parse_package_name (s));
        version v (parse_package_version (s));

        if (opt && check_dup (n, arg))
          continue;

        if (v.empty ())
          v = wildcard_version;

        const system_package* sp (system_repository.find (n));

        // Will deal with all the duplicates later.
        //
        if (sp == nullptr || !sp->authoritative)
          system_repository.insert (n, v, true);
      }
      else if (opt)
        warn << "no information can be extracted from ?" << s <<
          info << "package is ignored";
    }

    if (pargs.empty ())
    {
      warn << "nothing to build";
      return 0;
    }

    // Assemble the list of packages we will need to build.
    //
    build_packages pkgs;
    strings names;
    {
      transaction t (db.begin ());

      shared_ptr<repository> root (db.load<repository> (""));

      // Here is what happens here: are are going to try and guess whether we
      // are dealing with a package archive, package directory, or package
      // name/version by first trying it as an archive, then as a directory,
      // and then assume it is name/version. Sometimes, however, it is really
      // one of the first two but just broken. In this case things are really
      // confusing since we suppress all diagnostics for the first two
      // "guesses". So what we are going to do here is re-run them with full
      // diagnostics if the name/version guess doesn't pan out.
      //
      bool diag (false);
      for (auto i (pargs.cbegin ()); i != pargs.cend (); )
      {
        const char* package (i->c_str ());

        // Reduce all the potential variations (archive, directory, package
        // name, package name/version) to a single available_package object.
        //
        string n;
        version v;

        shared_ptr<repository> ar;
        shared_ptr<available_package> ap;

        bool sys (parse_package_scheme (package) == package_scheme::sys);

        if (!sys)
        {
          // Is this a package archive?
          //
          try
          {
            path a (package);
            if (exists (a))
            {
              if (diag)
                info << "'" << package << "' does not appear to be a valid "
                     << "package archive: ";

              package_manifest m (pkg_verify (o, a, true, diag));

              // This is a package archive (note that we shouldn't throw
              // failed from here on).
              //
              l4 ([&]{trace << "archive " << a;});
              n = m.name;
              v = m.version;
              ar = root;
              ap = make_shared<available_package> (move (m));
              ap->locations.push_back (
                package_location {root,
                                  string () /* fragment */,
                                  move (a)});
            }
          }
          catch (const invalid_path&)
          {
            // Not a valid path so cannot be an archive.
          }
          catch (const failed&)
          {
            // Not a valid package archive.
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
          if (pn != 0 && path::traits::is_separator (package[pn - 1]))
          {
            try
            {
              dir_path d (package);
              if (exists (d))
              {
                if (diag)
                  info << "'" << package << "' does not appear to be a valid "
                       << "package directory: ";

                package_manifest m (pkg_verify (d, true, diag));

                // Fix-up the package version to properly decide if we need to
                // upgrade/downgrade the package.
                //
                optional<version> pv (package_version (o, d));

                if (pv)
                  m.version = move (*pv);

                // This is a package directory (note that we shouldn't throw
                // failed from here on).
                //
                l4 ([&]{trace << "directory " << d;});
                n = m.name;
                v = m.version;
                ap = make_shared<available_package> (move (m));
                ar = root;
                ap->locations.push_back (
                  package_location {root,
                                    string () /* fragment */,
                                    move (d)});
              }
            }
            catch (const invalid_path&)
            {
              // Not a valid path so cannot be a package directory.
            }
            catch (const failed&)
            {
              // Not a valid package directory.
            }
          }
        }

        // If this was a diagnostics "run", then we are done.
        //
        if (diag)
          throw failed ();

        // Then it got to be a package name with optional version.
        //
        if (ap == nullptr)
        {
          try
          {
            n = parse_package_name (package);
            v = parse_package_version (package);

            l4 ([&]{trace << (sys ? "system " : "") << "package " << n
                          << "; version " << v;});

            // Either get the user-specified version or the latest for a
            // source code package. For a system package we peek the latest
            // one just to ensure the package is recognized.
            //
            auto rp (
              v.empty () || sys
              ? find_available (db, n, root, nullopt)
              : find_available (db, n, root, dependency_constraint (v)));

            ap = rp.first;
            ar = rp.second;
          }
          catch (const failed&)
          {
            diag = true;
            continue;
          }
        }

        // We are handling this argument.
        //
        if (check_dup (n, i++->c_str ()))
          continue;

        // Load the package that may have already been selected and
        // figure out what exactly we need to do here. The end goal
        // is the available_package object corresponding to the actual
        // package that we will be building (which may or may not be
        // the same as the selected package).
        //
        shared_ptr<selected_package> sp (db.find<selected_package> (n));

        if (sp != nullptr && sp->state == package_state::broken)
          fail << "unable to build broken package " << n <<
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
            if (!v.empty () &&
                find_available (db, n, root, nullopt).first != nullptr)
              sys_advise = true;
          }
          else if (ap->stub ())
          {
            sys_advise = true;
            ap = nullptr;
          }

          // If the user asked for a specific version, then that's what we
          // ought to be building.
          //
          if (!v.empty ())
          {
            for (;;)
            {
              if (ap != nullptr) // Must be that version, see above.
                break;

              // Otherwise, our only chance is that the already selected object
              // is that exact version.
              //
              if (sp != nullptr && !sp->system () && sp->version == v)
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
            assert (!sys);

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
          diag_record dr (fail);

          if (!sys_advise)
          {
            dr << "unknown package " << n;

            // Let's help the new user out here a bit.
            //
            if (db.query_value<repository_count> () == 0)
              dr << info << "configuration " << c << " has no repositories"
                 << info << "use 'bpkg rep-add' to add a repository";
            else if (db.query_value<available_package_count> () == 0)
              dr << info << "configuration " << c << " has no available "
                 << "packages"
                 << info << "use 'bpkg rep-fetch' to fetch available packages "
                 << "list";
          }
          else
          {
            dr << package << " is not available in source" <<
              info << "specify sys:" << package << " if it is available from "
               << "the system";
          }
        }

        // If the available_package object is still NULL, then it means
        // we need to get one corresponding to the selected package.
        //
        if (ap == nullptr)
        {
          assert (sp != nullptr && sp->system () == sys);

          auto rp (make_available (o, c, db, sp));
          ap = rp.first;
          ar = rp.second; // Could be NULL (orphan).
        }

        if (v.empty () && sys)
          v = wildcard_version;

        // We will keep the output directory only if the external package is
        // replaced with an external one. Note, however, that at this stage
        // the available package is not settled down yet, as we still need to
        // satisfy all the constraints. Thus the available package check is
        // postponed until the package disfiguring.
        //
        bool keep_out (o.keep_out () && sp->external ());

        // Finally add this package to the list.
        //
        build_package p {
          move (sp),
          move (ap),
          move (ar),
          true,          // Hold package.
          !v.empty (),   // Hold version.
          {},            // Constraints.
          sys,
          keep_out,
          {""},          // Required by (command line).
          false};        // Reconfigure.

        l4 ([&]{trace << "collect " << p.available_name ();});

        // "Fix" the version the user asked for by adding the '==' constraint.
        //
        // Note: for a system package this must always be present (so that
        // this build_package instance is never replaced).
        //
        if (!v.empty ())
          p.constraints.emplace_back (
            "command line",
            dependency_constraint (v));

        // Pre-collect user selection to make sure dependency-forced
        // up/down-grades are handled properly (i.e., the order in which we
        // specify packages on the command line does not matter).
        //
        pkgs.collect (o, c, db, move (p), false);
        names.push_back (n);
      }

      // Collect all the packages prerequisites.
      //
      for (const string& n: names)
        pkgs.collect_prerequisites (o, c, db, n);

      // Now that we have collected all the package versions that we need
      // to build, arrange them in the "dependency order", that is, with
      // every package on the list only possibly depending on the ones
      // after it. Iterate over the names we have collected on the previous
      // step in reverse so that when we iterate over the packages (also in
      // reverse), things will be built as close as possible to the order
      // specified by the user (it may still get altered if there are
      // dependencies between the specified packages).
      //
      for (const string& n: reverse_iterate (names))
        pkgs.order (n);

      // Finally, collect and order all the dependents that we will need
      // to reconfigure because of the up/down-grades of packages that
      // are now on the list.
      //
      pkgs.collect_order_dependents (db);

      t.commit ();
    }

    // Print what we are going to do, then ask for the user's confirmation.
    // While at it, detect if we have any dependents that the user may want to
    // update.
    //
    bool update_dependents (false);

    // Print the plan and ask for the user's confirmation only if some implicit
    // action (such as building prerequisite or reconfiguring dependent
    // package) to be taken or there is a selected package which version must
    // be changed.
    //
    string plan;
    bool print_plan (false);

    if (o.print_only () || !o.yes ())
    {
      for (const build_package& p: reverse_iterate (pkgs))
      {
        const shared_ptr<selected_package>& sp (p.selected);

        string act;
        string cause;
        if (p.available == nullptr)
        {
          // This is a dependent needing reconfiguration.
          //
          // This is an implicit reconfiguration which requires the plan to be
          // printed. Will flag that later when composing the list of
          // prerequisites.
          //
          assert (sp != nullptr && p.reconfigure ());
          update_dependents = true;
          act = "reconfigure " + sp->name;
          cause = "dependent of";
        }
        else
        {
          // Even if we already have this package selected, we have to
          // make sure it is configured and updated.
          //
          if (sp == nullptr)
            act = p.system ? "configure " : "build ";
          else if (sp->version == p.available_version ())
          {
            // If this package is already configured and is not part of the
            // user selection, then there is nothing we will be explicitly
            // doing with it (it might still get updated indirectly as part
            // of the user selection update).
            //
            if (!p.reconfigure () &&
                sp->state == package_state::configured &&
                !p.user_selection ())
              continue;

            act = p.system
              ? "reconfigure "
              : p.reconfigure ()
                ? "reconfigure/build "
                : "build ";
          }
          else
          {
            act = p.system
              ? "reconfigure "
              : sp->version < p.available_version ()
                ? "upgrade "
                : "downgrade ";

            print_plan = true;
          }

          act += p.available_name ();
          cause = "required by";
        }

        string rb;
        if (!p.user_selection ())
        {
          for (const string& n: p.required_by)
            rb += ' ' + n;

          // If not user-selected, then there should be another (implicit)
          // reason for the action.
          //
          assert (!rb.empty ());

          print_plan = true;
        }

        if (!rb.empty ())
          act += " (" + cause + rb + ')';

        if (o.print_only ())
          cout << act << endl;
        else if (verb)
          // Print indented for better visual separation.
          //
          plan += (plan.empty () ? "  " : "\n  ") + act;
      }
    }

    if (o.print_only ())
      return 0;

    if (print_plan)
      text << plan;

    // Ask the user if we should continue.
    //
    if (!(o.yes () || !print_plan || yn_prompt ("continue? [Y/n]", 'y')))
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
    // 5.  build           user selection               [right to left]
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
    // Almost forgot, there is one more thing: when we upgrade or downgrade a
    // package, it may change the list of its prerequisites. Which means we
    // may end up with packages that are no longer necessary and it would be
    // nice to offer to drop those. This, howeve, is a tricky business and is
    // the domain of pkg_drop(). For example, a prerequisite may still have
    // other dependents (so it looks like we shouldn't be dropping it) but
    // they are all from the "drop set" (so we should offer to drop it after
    // all); pkg_drop() knows how to deal with all this.
    //
    // So what we are going to do is this: before disfiguring packages we will
    // collect all their old prerequisites. This will be the "potentially to
    // drop" list. Then, after configuration, when the new dependencies are
    // established, we will pass them to pkg_drop() whose job will be to
    // figure out which ones can be dropped, prompt the user, etc.
    //
    // We also have the other side of this logic: dependent packages that we
    // reconfigure because their prerequsites got upgraded/downgraded and that
    // the user may want to in addition update (that update_dependents flag
    // above). This case we handle in house.
    //
    set<shared_ptr<selected_package>> drop_pkgs;

    // disfigure
    //
    for (build_package& p: pkgs)
    {
      // We are only interested in configured packages that are either
      // up/down-graded or need reconfiguration (e.g., dependents).
      //
      if (!p.reconfigure ())
        continue;

      shared_ptr<selected_package>& sp (p.selected);

      // Each package is disfigured in its own transaction, so that we
      // always leave the configuration in a valid state.
      //
      transaction t (db.begin ());

      // Collect prerequisites to be potentially dropped.
      //
      if (!o.keep_prerequisite ())
      {
        for (const auto& pair: sp->prerequisites)
        {
          shared_ptr<selected_package> pp (pair.first.load ());

          if (!pp->hold_package)
            drop_pkgs.insert (move (pp));
        }
      }

      // Reset the flag if the package being unpacked is not an external one.
      //
      if (p.keep_out)
      {
        const shared_ptr<available_package>& ap (p.available);
        const package_location& pl (ap->locations[0]);

        if (pl.repository.object_id () == "") // Special root.
          p.keep_out = !exists (pl.location); // Directory case.
        else
        {
          p.keep_out = false;

          // See if the package comes from the directory-based repository, and
          // so is external.
          //
          // Note that such repositories are always preferred over others (see
          // below).
          //
          for (const package_location& l: ap->locations)
          {
            if (l.repository.load ()->location.directory_based ())
            {
              p.keep_out = true;
              break;
            }
          }
        }
      }

      pkg_disfigure (c, o, t, sp, !p.keep_out); // Commits the transaction.
      assert (sp->state == package_state::unpacked ||
              sp->state == package_state::transient);

      if (verb)
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

        sp.reset ();
      }
    }

    // purge, fetch/unpack|checkout, configure
    //
    for (build_package& p: reverse_iterate (pkgs))
    {
      shared_ptr<selected_package>& sp (p.selected);
      const shared_ptr<available_package>& ap (p.available);

      // Purge the system package, fetch/unpack or checkout the source one.
      //
      for (;;) // Breakout loop.
      {
        if (ap == nullptr) // Skip dependents.
          break;

        // System package should not be fetched, it should only be configured
        // on the next stage. Here we need to purge selected non-system package
        // if present. Before we drop the object we need to make sure the hold
        // state is preserved for the package being reconfigured.
        //
        if (p.system)
        {
          if (sp != nullptr && !sp->system ())
          {
            transaction t (db.begin ());
            pkg_purge (c, t, sp); // Commits the transaction.

            if (verb)
              text << "purged " << *sp;

            if (!p.hold_package)
              p.hold_package = sp->hold_package;

            if (!p.hold_version)
              p.hold_version = sp->hold_version;

            sp.reset ();
          }

          break;
        }

        // Fetch or checkout if this is a new package or if we are
        // up/down-grading.
        //
        if (sp == nullptr || sp->version != p.available_version ())
        {
          sp.reset (); // For the directory case below.

          // Distinguish between the package and archive/directory cases.
          //
          const package_location& pl (ap->locations[0]); // Got to have one.

          if (pl.repository.object_id () != "") // Special root?
          {
            transaction t (db.begin ());

            // Go through package repositories to decide if we should fetch,
            // checkout or unpack depending on the available repository basis.
            // Preferring a local one over the remotes and the dir repository
            // type over the others seems like a sensible thing to do.
            //
            optional<repository_basis> basis;

            for (const package_location& l: ap->locations)
            {
              const repository_location& rl (l.repository.load ()->location);

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
                                true /* replace */);
                break;
              }
            case repository_basis::version_control:
              {
                sp = pkg_checkout (o,
                                   c,
                                   t,
                                   ap->id.name,
                                   p.available_version (),
                                   true /* replace */);
                break;
              }
            case repository_basis::directory:
              {
                sp = pkg_unpack (c,
                                 t,
                                 ap->id.name,
                                 p.available_version (),
                                 true /* replace */);
                break;
              }
            }
          }
          // Directory case is handled by unpack.
          //
          else if (exists (pl.location))
          {
            transaction t (db.begin ());
            sp = pkg_fetch (
              o,
              c,
              t,
              pl.location, // Archive path.
              true,        // Replace
              false);      // Don't purge; commits the transaction.
          }

          if (sp != nullptr) // Actually fetched or checked out something?
          {
            assert (sp->state == package_state::fetched ||
                    sp->state == package_state::unpacked);

            if (verb)
            {
              const repository_location& rl (sp->repository);

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
            transaction t (db.begin ());
            sp = pkg_unpack (o, c, t, ap->id.name); // Commits the transaction.

            if (verb)
              text << "unpacked " << *sp;
          }
          else
          {
            const package_location& pl (ap->locations[0]);
            assert (pl.repository.object_id () == ""); // Special root.

            transaction t (db.begin ());
            sp = pkg_unpack (o,
                             c,
                             t,
                             path_cast<dir_path> (pl.location),
                             true,   // Replace.
                             false); // Don't purge; commits the transaction.

            if (verb)
              text << "using " << *sp << " (external)";
          }

          assert (sp->state == package_state::unpacked);
        }

        break; // Get out from the breakout loop.
      }

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

      transaction t (db.begin ());

      // Note that pkg_configure() commits the transaction.
      //
      if (p.system)
        sp = pkg_configure_system (ap->id.name, p.available_version (), t);
      else
        pkg_configure (c, o, t, sp, strings ());

      assert (sp->state == package_state::configured);

      if (verb)
        text << "configured " << *sp;
    }

    // Small detour: update the hold state. While we could have tried
    // to "weave" it into one of the previous actions, things there
    // are already convoluted enough.
    //
    for (const build_package& p: reverse_iterate (pkgs))
    {
      const shared_ptr<selected_package>& sp (p.selected);
      assert (sp != nullptr);

      // Note that we should only "increase" the hold_package state. For
      // version, if the user requested upgrade to the (unspecified) latest,
      // then we want to reset it.
      //
      bool hp (p.hold_package ? *p.hold_package : sp->hold_package);
      bool hv (p.hold_version ? *p.hold_version : sp->hold_version);

      if (hp != sp->hold_package || hv != sp->hold_version)
      {
        sp->hold_package = hp;
        sp->hold_version = hv;

        transaction t (db.begin ());
        db.update (sp);
        t.commit ();

        // Clean up if this package ended up in the potention drop set.
        //
        if (hp)
        {
          auto i (drop_pkgs.find (sp));
          if (i != drop_pkgs.end ())
            drop_pkgs.erase (i);
        }

        if (verb > 1)
        {
          if (hp)
            text << "hold package " << sp->name;

          if (hv)
            text << "hold version " << *sp;
        }
      }
    }

    // Now that we have the final dependency state, see if we need to drop
    // packages that are no longer necessary.
    //
    if (!drop_pkgs.empty ())
      drop_pkgs = pkg_drop (
        c, o, db, drop_pkgs, !(o.yes () || o.drop_prerequisite ()));

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
      const shared_ptr<selected_package>& sp (p.selected);

      if (!sp->system () && // System package doesn't need update.
          p.user_selection ())
        upkgs.push_back (pkg_command_vars {sp, strings ()});
    }

    // Then add dependents. We do it as a separate step so that they are
    // updated after the user selection.
    //
    if (update_dependents)
    {
      for (const build_package& p: reverse_iterate (pkgs))
      {
        const shared_ptr<selected_package>& sp (p.selected);

        if (p.reconfigure () && p.available == nullptr)
        {
          // Note that it is entirely possible this package got dropped so
          // we need to check for that.
          //
          if (drop_pkgs.find (sp) == drop_pkgs.end ())
            upkgs.push_back (pkg_command_vars {sp, strings ()});
        }
      }
    }

    pkg_update (c, o, o.for_ (), strings (), upkgs);

    if (verb)
    {
      for (const pkg_command_vars& pv: upkgs)
        text << "updated " << *pv.pkg;
    }

    return 0;
  }
}
