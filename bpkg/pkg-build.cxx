// file      : bpkg/pkg-build.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-build>

#include <map>
#include <set>
#include <list>
#include <cstring>    // strlen()
#include <iostream>   // cout
#include <algorithm>  // find()

#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/database>
#include <bpkg/diagnostics>
#include <bpkg/satisfaction>
#include <bpkg/manifest-utility>

#include <bpkg/common-options>

#include <bpkg/pkg-drop>
#include <bpkg/pkg-fetch>
#include <bpkg/pkg-unpack>
#include <bpkg/pkg-update>
#include <bpkg/pkg-verify>
#include <bpkg/pkg-configure>
#include <bpkg/pkg-disfigure>

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
  // NULL for both if not found.
  //
  pair<shared_ptr<available_package>, shared_ptr<repository>>
  find_available (database& db,
                  const string& name,
                  const shared_ptr<repository>& r,
                  const optional<dependency_constraint>& c)
  {
    using query = query<available_package>;

    query q (query::id.name == name);
    const auto& vm (query::id.version);

    // If there is a constraint, then translate it to the query. Otherwise,
    // get the latest version.
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
      if (c->min_version &&
          c->max_version &&
          *c->min_version == *c->max_version)
      {
        const version& v (*c->min_version);

        q = q && compare_version_eq (vm, v, v.revision != 0);

        if (v.revision == 0)
          q += order_by_revision_desc (vm);
      }
      else
      {
        if (c->min_version)
        {
          const version& v (*c->min_version);

          if (c->min_open)
            q = q && compare_version_gt (vm, v, v.revision != 0);
          else
            q = q && compare_version_ge (vm, v, v.revision != 0);
        }

        if (c->max_version)
        {
          const version& v (*c->max_version);

          if (c->max_open)
            q = q && compare_version_lt (vm, v, v.revision != 0);
          else
            q = q && compare_version_le (vm, v, v.revision != 0);
        }

        q += order_by_version_desc (vm);
      }
    }
    else
      q += order_by_version_desc (vm);

    // Filter the result based on the repository to which each version
    // belongs.
    //
    return filter_one (r, db.query<available_package> (q));
  }

  // Create a transient (or fake, if you prefer) available_package
  // object corresponding to the specified selected object. Note
  // that the package locations list is left empty and that the
  // returned repository could be NULL if the package is an orphan.
  //
  pair<shared_ptr<available_package>, shared_ptr<repository>>
  make_available (const common_options& options,
                  const dir_path& cd,
                  database& db,
                  const shared_ptr<selected_package>& sp)
  {
    assert (sp != nullptr && sp->state != package_state::broken);

    // First see if we can find its repository.
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
         selected->version != available->version);
    }
  };

  struct build_packages: list<reference_wrapper<build_package>>
  {
    // Collect the package. Return true if this package version was,
    // in fact, added to the map and false if it was already there
    // or the existing version was preferred.
    //
    bool
    collect (const common_options& options,
             const dir_path& cd,
             database& db,
             build_package&& pkg)
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

        // If versions are the same, then all we have to do is copy the
        // constraint (p1/p2 already point to where we would want them to).
        //
        if (p1->available->version != p2->available->version)
        {
          using constraint_type = build_package::constraint_type;

          // If the versions differ, we have to pick one. Start with the
          // newest version since if both satisfy, then that's the one we
          // should prefer. So get the first to try into p1 and the second
          // to try -- into p2.
          //
          if (p2->available->version > p1->available->version)
            swap (p1, p2);

          // See if pv's version satisfies pc's constraints. Return the
          // pointer to the unsatisfied constraint or NULL if all are
          // satisfied.
          //
          auto test = [] (build_package* pv, build_package* pc)
            -> const constraint_type*
          {
            for (const constraint_type& c: pc->constraints)
              if (!satisfies (pv->available->version, c.value))
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
                info << "available " << n << " " << p1->available->version <<
                info << "available " << n << " " << p2->available->version <<
                info << "explicitly specify " << n << " version to manually "
                   << "satisfy both constraints";
            }
            else
              swap (p1, p2);
          }

          l4 ([&]{trace << "pick " << n << " " << p1->available->version
                        << " over " << p2->available->version;});
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

        if (!replace)
          return false;
      }
      else
      {
        // This is the first time we are adding this package name to the map.
        //
        string n (pkg.available->id.name); // Note: copy; see emplace() below.

        l4 ([&]{trace << "add " << n << " " << pkg.available->version;});

        i = map_.emplace (move (n), data_type {end (), move (pkg)}).first;
      }

      // Now collect all the prerequisites recursively. But first "prune"
      // this process if the package is already configured since that would
      // mean all its prerequisites are configured as well. Note that this
      // is not merely an optimization: the package could be an orphan in
      // which case the below logic will fail (no repository in which to
      // search for prerequisites). By skipping the prerequisite check we
      // are able to gracefully handle configured orphans.
      //
      const build_package& p (i->second.package);
      const shared_ptr<selected_package>& sp (p.selected);
      const shared_ptr<available_package>& ap (p.available);

      if (sp != nullptr &&
          sp->version == ap->version &&
          sp->state == package_state::configured)
        return true;

      // Show how we got here if things go wrong.
      //
      auto g (
        make_exception_guard (
          [&ap] ()
          {
            info << "while satisfying " << ap->id.name << " " << ap->version;
          }));

      const shared_ptr<repository>& ar (p.repository);
      const string& name (ap->id.name);

      for (const dependency_alternatives& da: ap->dependencies)
      {
        if (da.conditional) // @@ TODO
          fail << "conditional dependencies are not yet supported";

        if (da.size () != 1) // @@ TODO
          fail << "multiple dependency alternatives not yet supported";

        const dependency& d (da.front ());

        // The first step is to always find the available package even
        // if, in the end, it won't be the one we select. If we cannot
        // find the package then that means the repository is broken.
        // And if we have no repository to look in, then that means the
        // package is an orphan (we delay this check until we actually
        // need the repository to allow orphans without prerequisites).
        //
        if (ar == nullptr)
          fail << "package " << name << " " << ap->version << " is orphaned" <<
            info << "explicitly upgrade it to a new version";

        auto rp (find_available (db, d.name, ar, d.constraint));

        if (rp.first == nullptr)
        {
          diag_record dr;
          dr << fail << "unknown prerequisite " << d << " of package " << name;

          if (!ar->location.empty ())
            dr << info << "repository " << ar->location << " appears to "
                       << "be broken";
        }

        // Next see if this package is already selected. If we already
        // have it in the configuraion and it satisfies our dependency
        // constraint, then we don't want to be forcing its upgrade (or,
        // worse, downgrade).
        //
        bool force (false);
        shared_ptr<selected_package> dsp (db.find<selected_package> (d.name));
        if (dsp != nullptr)
        {
          if (dsp->state == package_state::broken)
            fail << "unable to build broken package " << d.name <<
              info << "use 'pkg-purge --force' to remove";

          if (satisfies (dsp->version, d.constraint))
            rp = make_available (options, cd, db, dsp);
          else
            // Remember that we may be forcing up/downgrade; we will deal
            // with it below.
            //
            force = true;
        }

        build_package dp {
          dsp,
          rp.first,
          rp.second,
          nullopt,  // Hold package.
          nullopt,  // Hold version.
          {},       // Constraints.
          {name},   // Required by.
          false};   // Reconfigure.

        // Add our constraint, if we have one.
        //
        if (d.constraint)
          dp.constraints.emplace_back (name, *d.constraint);

        // Now collect this prerequisite. If it was actually collected
        // (i.e., it wasn't already there) and we are forcing an upgrade
        // and the version is not held, then warn, unless we are running
        // quiet. Downgrade or upgrade of a held version -- refuse.
        //
        if (collect (options, cd, db, move (dp)) && force)
        {
          const version& sv (dsp->version);
          const version& av (rp.first->version);

          // Fail if downgrade or held.
          //
          bool u (av > sv);
          bool f (dsp->hold_version || !u);

          if (verb || f)
          {
            bool c (d.constraint);
            diag_record dr;

            (f ? dr << fail : dr << warn)
              << "package " << name << " dependency on "
              << (c ? "(" : "") << d << (c ? ")" : "") << " is forcing "
              << (u ? "up" : "down") << "grade of " << d.name << " " << sv
              << " to " << av;

            if (dsp->hold_version)
              dr << info << "package version " << d.name << " " << sv
                 << " is held";

            if (f)
              dr << info << "explicitly request version "
                 << (u ? "up" : "down") << "grade to continue";
          }
        }
      }

      return true;
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
      // configured, right? Not so fast. While in collect() we didn't
      // need to add prerequisites of such a package, it doesn't mean
      // that they actually never ended up in the map via another way.
      // For example, some can be a part of the initial selection. And
      // in that case we must order things properly.
      //
      // So here we are going to do things differently depending on
      // whether the package is already configured or not. If it is,
      // then that means we can use its prerequisites list. Otherwise,
      // we use the manifest data.
      //
      if (sp != nullptr &&
          sp->version == ap->version &&
          sp->state == package_state::configured)
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
               reverse_iterate (p.available->dependencies))
        {
          assert (!da.conditional && da.size () == 1); // @@ TODO
          const dependency& d (da.front ());

          update (order (d.name, false));
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
      const shared_ptr<available_package>& ap (p.available);

      const string& n (sp->name);

      // See if we are up/downgrading this package. In particular, the
      // available package could be NULL meaning we are just reconfiguring.
      //
      int ud (ap != nullptr ? sp->version.compare (ap->version) : 0);

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
            dp.selected->version == dp.available->version;
        }

        if (check)
        {
          const version& v (ap->version);
          const dependency_constraint& c (*pd.constraint);

          if (!satisfies (v, c))
          {
            diag_record dr;

            dr << fail << "unable to " << (ud < 0 ? "up" : "down") << "grade "
               << "package " << n << " " << sp->version << " to " << v;

            dr << info << "because package " << dn << " depends on (" << n
               << " " << c << ")";

            string rb;
            if (p.required_by.find ("") == p.required_by.end ())
            {
              for (const string& n: p.required_by)
                rb += ' ' + n;
            }

            if (!rb.empty ())
              dr << info << "package " << n << " " << v << " required by"
                 << rb;

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

          i = map_.emplace (
            move (dn),
            data_type
            {
              end (),
              build_package {
                move (dsp),
                nullptr,
                nullptr,
                nullopt,  // Hold package.
                nullopt,  // Hold version.
                {},       // Constraints.
                {n},      // Required by.
                true}     // Reconfigure.
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
  pkg_build (const pkg_build_options& o, cli::scanner& args)
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

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-build' for more information";

    database db (open (c, trace));

    // Note that the session spans all our transactions. The idea here is
    // that selected_package objects in the build_packages list below will
    // be cached in this session. When subsequent transactions modify any
    // of these objects, they will modify the cached instance, which means
    // our list will always "see" their updated state.
    //
    session s;

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
      for (bool diag (false); args.more (); )
      {
        const char* s (args.peek ());
        size_t sn (strlen (s));

        // Reduce all the potential variations (archive, directory, package
        // name, package name/version) to a single available_package object.
        //
        string n;
        version v;

        shared_ptr<repository> ar;
        shared_ptr<available_package> ap;

        // Is this a package archive?
        //
        try
        {
          path a (s);
          if (exists (a))
          {
            if (diag)
              info << "'" << s << "' does not appear to be a valid package "
                   << "archive: ";

            package_manifest m (pkg_verify (o, a, true, diag));

            // This is a package archive (note that we shouldn't throw
            // failed from here on).
            //
            l4 ([&]{trace << "archive " << a;});
            n = m.name;
            v = m.version;
            ar = root;
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
        if (sn != 0 && path::traits::is_separator (s[sn - 1]))
        {
          try
          {
            dir_path d (s);
            if (exists (d))
            {
              if (diag)
                info << "'" << s << "' does not appear to be a valid package "
                     << "directory: ";

              package_manifest m (pkg_verify (d, true, diag));

              // This is a package directory (note that we shouldn't throw
              // failed from here on).
              //
              l4 ([&]{trace << "directory " << d;});
              n = m.name;
              v = m.version;
              ap = make_shared<available_package> (move (m));
              ar = root;
              ap->locations.push_back (package_location {root, move (d)});
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
            n = parse_package_name (s);
            v = parse_package_version (s);
            l4 ([&]{trace << "package " << n << "; version " << v;});

            // Either get the user-specified version or the latest.
            //
            auto rp (
              v.empty ()
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

        args.next (); // We are handling this argument.

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

        // If the user asked for a specific version, then that's what
        // we ought to be building.
        //
        if (!v.empty ())
        {
          for (;;)
          {
            if (ap != nullptr) // Must be that version, see above.
              break;

            // Otherwise, our only chance is that the already selected
            // object is that exact version.
            //
            if (sp != nullptr && sp->version == v)
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
            // Even if this package is already in the configuration, should
            // we have a newer version, we treat it as an upgrade request;
            // otherwise, why specify the package in the first place? We just
            // need to check if what we already have is "better" (i.e., newer).
            //
            if (sp != nullptr && ap->id.version < sp->version)
              ap = nullptr; // Derive ap from sp below.
          }
          else
          {
            if (sp == nullptr)
              found = false;

            // Otherwise, derive ap from sp below.
          }
        }

        if (!found)
        {
          diag_record dr;

          dr << fail << "unknown package " << n;
          if (!v.empty ())
            dr << " " << v;

          // Let's help the new user out here a bit.
          //
          if (db.query_value<repository_count> () == 0)
            dr << info << "configuration " << c << " has no repositories"
               << info << "use 'bpkg cfg-add' to add a repository";
          else if (db.query_value<available_package_count> () == 0)
            dr << info << "configuration " << c << " has no available packages"
               << info << "use 'bpkg cfg-fetch' to fetch available packages "
               << "list";
        }

        // If the available_package object is still NULL, then it means
        // we need to get one corresponding to the selected package.
        //
        if (ap == nullptr)
        {
          assert (sp != nullptr);

          auto rp (make_available (o, c, db, sp));
          ap = rp.first;
          ar = rp.second; // Could be NULL (orphan).
        }

        // Finally add this package to the list.
        //
        l4 ([&]{trace << "collect " << ap->id.name << " " << ap->version;});

        build_package p {
          move (sp),
          move (ap),
          move (ar),
          true,         // Hold package.
          !v.empty (),  // Hold version.
          {},           // Constraints.
          {""},         // Required by (command line).
          false};       // Reconfigure.

        // "Fix" the version the user asked for by adding the '==' constraint.
        //
        if (!v.empty ())
          p.constraints.emplace_back (
            "command line",
            dependency_constraint (v));

        pkgs.collect (o, c, db, move (p));
        names.push_back (n);
      }

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

    if (o.print_only () || !o.yes ())
    {
      for (const build_package& p: reverse_iterate (pkgs))
      {
        const shared_ptr<selected_package>& sp (p.selected);
        const shared_ptr<available_package>& ap (p.available);

        string act;
        string cause;
        if (ap == nullptr)
        {
          // This is a dependent needing reconfiguration.
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
            act = "build ";
          else if (sp->version == ap->version)
          {
            // If this package is already configured and is not part of the
            // user selection, then there is nothing we will be explicitly
            // doing with it (it might still get updated indirectly as part
            // of the user selection update).
            //
            if (!p.reconfigure () &&
                sp->state == package_state::configured &&
                find (names.begin (), names.end (), sp->name) == names.end ())
              continue;

            act = p.reconfigure () ? "reconfigure/build " : "build ";
          }
          else
            act = sp->version < ap->version ? "upgrade " : "downgrade ";

          act += ap->id.name + ' ' + ap->version.string ();
          cause = "required by";
        }

        string rb;
        if (p.required_by.find ("") == p.required_by.end ()) // User selection?
        {
          for (const string& n: p.required_by)
            rb += ' ' + n;
        }

        if (!rb.empty ())
          act += " (" + cause + rb + ')';

        if (o.print_only ())
          cout << act << endl;
        else if (verb)
          text << act;
      }
    }

    if (o.print_only ())
      return 0;

    // Ask the user if we should continue.
    //
    if (!(o.yes () || yn_prompt ("continue? [Y/n]", 'y')))
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
    // 1. disfigure  up/down-graded, reconfigured [left to right]
    // 2. purge      up/down-graded
    // 3. fetch      new, up/down-graded
    // 4. unpack     new, up/down-graded
    // 5. configure  all                          [right to left]
    // 6. build      user selection               [right to left]
    //
    // Note that for some actions, e.g., purge or fetch, the order is not
    // really important. We will, however, do it right to left since that
    // is the order closest to that of the user selection.
    //
    // We are also going to combine purge/fetch/unpack into a single step
    // and use the replace mode so it will become just fetch/unpack.
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
    for (const build_package& p: pkgs)
    {
      // We are only interested in configured packages that are either
      // up/down-graded or need reconfiguration (e.g., dependents).
      //
      if (!p.reconfigure ())
        continue;

      const shared_ptr<selected_package>& sp (p.selected);

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

      pkg_disfigure (c, o, t, sp); // Commits the transaction.
      assert (sp->state == package_state::unpacked);

      if (verb)
        text << "disfigured " << sp->name << " " << sp->version;
    }

    // fetch/unpack
    //
    for (build_package& p: reverse_iterate (pkgs))
    {
      shared_ptr<selected_package>& sp (p.selected);
      const shared_ptr<available_package>& ap (p.available);

      if (ap == nullptr) // Skip dependents.
        continue;

      // Fetch if this is a new package or if we are up/down-grading.
      //
      if (sp == nullptr || sp->version != ap->version)
      {
        sp.reset (); // For the directory case below.

        // Distinguish between the package and archive/directory cases.
        //
        const package_location& pl (ap->locations[0]); // Got to have one.

        if (pl.repository.object_id () != "") // Special root?
        {
          transaction t (db.begin ());
          sp = pkg_fetch (o,
                          c,
                          t,
                          ap->id.name,
                          ap->version,
                          true); // Replace; commits the transaction.
        }
        else if (exists (pl.location)) // Directory case is handled by unpack.
        {
          transaction t (db.begin ());
          sp = pkg_fetch (o,
                          c,
                          t,
                          pl.location, // Archive path.
                          true,        // Replace
                          false);      // Don't purge; commits the transaction.
        }

        if (sp != nullptr) // Actually unpacked something?
        {
          assert (sp->state == package_state::fetched);

          if (verb)
            text << "fetched " << sp->name << " " << sp->version;
        }
      }

      // Unpack. Note that the package can still be NULL if this is the
      // directory case (see the fetch code above).
      //
      if (sp == nullptr || sp->state == package_state::fetched)
      {
        if (sp != nullptr)
        {
          transaction t (db.begin ());
          sp = pkg_unpack (o, c, t, ap->id.name); // Commits the transaction.
        }
        else
        {
          const package_location& pl (ap->locations[0]);
          assert (pl.repository.object_id () == ""); // Special root.

          transaction t (db.begin ());
          sp = pkg_unpack (c,
                           t,
                           path_cast<dir_path> (pl.location),
                           true,   // Replace.
                           false); // Don't purge; commits the transaction.
        }

        assert (sp->state == package_state::unpacked);

        if (verb)
          text << "unpacked " << sp->name << " " << sp->version;
      }
    }

    // configure
    //
    for (const build_package& p: reverse_iterate (pkgs))
    {
      const shared_ptr<selected_package>& sp (p.selected);

      assert (sp != nullptr);

      // We configure everything that isn't already configured.
      //
      if (sp->state == package_state::configured)
        continue;

      transaction t (db.begin ());
      pkg_configure (c, o, t, sp, strings ()); // Commits the transaction.
      assert (sp->state == package_state::configured);

      if (verb)
        text << "configured " << sp->name << " " << sp->version;
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
      // version, if the user requested upgrade to the (unsepcified) latest,
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
            text << "hold version " << sp->name << " " << sp->version;
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

      if (find (names.begin (), names.end (), sp->name) != names.end ())
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

    pkg_update (c, o, strings (), upkgs);

    if (verb)
    {
      for (const pkg_command_vars& pv: upkgs)
        text << "updated " << pv.pkg->name << " " << pv.pkg->version;
    }

    return 0;
  }
}
