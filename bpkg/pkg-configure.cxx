// file      : bpkg/pkg-configure.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-configure.hxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/file.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/operation.hxx>
#include <libbuild2/config/operation.hxx>

#include <bpkg/bpkg.hxx> // build2_init(), etc

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/satisfaction.hxx>
#include <bpkg/package-query.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/pkg-verify.hxx>
#include <bpkg/pkg-disfigure.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  static optional<version_constraint> absent_constraint;

  configure_prerequisites_result
  pkg_configure_prerequisites (const common_options& o,
                               database& db,
                               transaction&,
                               const dependencies& deps,
                               const vector<size_t>* alts,
                               package_skeleton&& ps,
                               const vector<package_name>* prev_prereqs,
                               bool simulate,
                               const function<find_database_function>& fdb,
                               const function<find_package_state_function>& fps,
                               const vector<package_key>* unconstrain_deps)
  {
    tracer trace ("pkg_configure_prerequisites");

    // Unconstraining dependencies are only allowed in the simulation mode.
    //
    assert (unconstrain_deps == nullptr || simulate);

    // No use case for both being specified.
    //
    assert (alts == nullptr || prev_prereqs == nullptr);

    tracer_guard tg (db, trace);

    package_prerequisites prereqs;
    vector<size_t>        dep_alts;
    strings               vars;

    // Notes on the buildfile clauses evaluation:
    //
    // - In the manual configuration mode (alts == NULL, prev_prereqs == NULL)
    //   we always evaluate the enable and reflect clauses. We, however, fail
    //   if any of the prefer or require clauses are specified in any of the
    //   enabled dependency alternatives, assuming that this package didn't
    //   negotiate its preferences/requirements for the dependency
    //   configurations.
    //
    //   Note that evaluating the require and prefer clauses in this case is
    //   meaningless since we don't reconfigure the dependencies nor negotiate
    //   configurations with other dependents. What we should probably do is
    //   load configurations of the dependencies and use them while evaluating
    //   the dependent's enable and reflect clauses as we go along. Probably
    //   we should still evaluate the accept clauses to make sure that the
    //   dependency is configured acceptably for the dependent.
    //
    // - In the pre-selected alternatives mode (alts != NULL, prev_prereqs ==
    //   NULL) we don't evaluate the enable, prefer, and require clauses since
    //   they have already been evaluated as a part of the dependency
    //   alternatives selection and the dependency configurations negotiation.
    //   We, however always evaluate the reflect clauses.
    //
    // - In the reconfiguration mode (prev_prereqs != NULL, alts == NULL) we
    //   don't evaluate the prefer and require clauses, assuming that was done
    //   on some previous pkg-build run when this package and its dependencies
    //   have been configured. But because of this we may not evaluate the
    //   enable and reflect clauses which refer to dependency configuration
    //   variables. If such clauses are present, then this is considered an
    //   implementation error since such packages should be handled in the
    //   above pre-selected alternatives mode.
    //
    bool manual (alts == nullptr && prev_prereqs == nullptr);

    // In the reconfiguration mode keep track of configuration variable
    // prefixes (in the 'config.<dependency>.' form) for dependencies in the
    // selected alternatives with the prefer or require clauses specified and
    // fail if any enable or reflect clause refers to them.
    //
    // Note that the enable and reflect clauses may only refer to dependency
    // configuration variables of already selected alternatives with the
    // prefer or require clauses specified.
    //
    vector<string> banned_var_prefixes;

    auto verify_banned_vars = [&ps,
                               &banned_var_prefixes] (const string& clause,
                                                      const char* what)
    {
      for (const string& p: banned_var_prefixes)
      {
        if (clause.find (p) != string::npos)
        {
          fail << "unable to reconfigure dependent " << ps.package.name
               << " with " << what << " clause that refers to dependency "
               << "configuration variables" <<
          info << "please report in https://github.com/build2/build2/issues/302";
        }
      }
    };

    // Alternatives argument must be parallel to the dependencies argument if
    // specified.
    //
    assert (alts == nullptr || alts->size () == deps.size ());

    dep_alts.reserve (deps.size ());

    for (size_t di (0); di != deps.size (); ++di)
    {
      // Skip the toolchain build-time dependencies and dependencies without
      // enabled alternatives.
      //
      const dependency_alternatives_ex& das (deps[di]);

      if (das.empty ())
      {
        dep_alts.push_back (0);
        continue;
      }

      small_vector<pair<reference_wrapper<const dependency_alternative>,
                        size_t>,
                   2> edas;

      if (alts == nullptr)
      {
        if (toolchain_buildtime_dependency (o, das, &ps.package.name))
        {
          dep_alts.push_back (0);
          continue;
        }

        for (size_t i (0); i != das.size (); ++i)
        {
          const dependency_alternative& da (das[i]);

          // Evaluate the dependency alternative enable clause, if present,
          // unless it refers to any banned variables in which case we fail.
          //
          if (da.enable)
          {
            if (!banned_var_prefixes.empty ())
              verify_banned_vars (*da.enable, "enable");

            if (!ps.evaluate_enable (*da.enable, make_pair (di, i)))
              continue;
          }

          if (manual && (da.prefer || da.require))
            fail << "manual configuration of dependents with prefer or "
                 << "require clauses is not yet supported";

          edas.push_back (make_pair (ref (da), i));
        }

        if (edas.empty ())
        {
          dep_alts.push_back (0);
          continue;
        }
      }
      else
      {
        // Must only contain the selected alternative.
        //
        assert (das.size () == 1);

        edas.push_back (make_pair (ref (das.front ()), (*alts)[di]));
      }

      // Pick the first alternative with dependencies that can all be resolved
      // to the configured packages, satisfying the respective constraints.
      //
      // If the list of the former prerequisites is specified, then first try
      // to select an alternative in the "recreate dependency decisions" mode,
      // filtering out alternatives where dependencies do not all belong to
      // this list. If we end up with no alternative selected, then retry in
      // the "make dependency decisions" mode and select the alternative
      // regardless of the former prerequisites.
      //
      assert (!edas.empty ());

      for (const vector<package_name>* pps (prev_prereqs);;)
      {
        const pair<reference_wrapper<const dependency_alternative>,
                   size_t>* selected_alt (nullptr);

        for (const auto& eda: edas)
        {
          const dependency_alternative& da (eda.first);

          // Cache the selected packages which correspond to the alternative
          // dependencies, pairing them with the respective constraints. If
          // the alternative turns out to be fully resolvable, we will add the
          // cached packages into the dependent's prerequisites map.
          //
          small_vector<
            pair<lazy_shared_ptr<selected_package>, prerequisite_info>,
            1> prerequisites;

          dependency_alternative::const_iterator b (da.begin ());
          dependency_alternative::const_iterator i (b);
          dependency_alternative::const_iterator e (da.end ());

          assert (b != e);

          for (; i != e; ++i)
          {
            const dependency&   d (*i);
            const package_name& n (d.name);

            database* ddb (fdb ? fdb (db, n, das.buildtime) : nullptr);

            pair<shared_ptr<selected_package>, database*> spd (
              ddb != nullptr
              ? make_pair (ddb->find<selected_package> (n), ddb)
              : find_dependency (db, n, das.buildtime));

            const shared_ptr<selected_package>& dp (spd.first);

            if (dp == nullptr)
              break;

            database& pdb (*spd.second);

            optional<pair<package_state, package_substate>> dps;
            if (fps != nullptr)
              dps = fps (dp);

            const optional<version_constraint>* dc (&d.constraint);

            // Unconstrain this dependency, if requested.
            //
            if (unconstrain_deps != nullptr)
            {
              const vector<package_key>& uds (*unconstrain_deps);
              if (find (uds.begin (), uds.end (), package_key (pdb, n)) !=
                  uds.end ())
              {
                dc = &absent_constraint;
              }
            }

            if ((dps ? dps->first : dp->state) != package_state::configured ||
                !satisfies (dp->version, *dc)                               ||
                (pps != nullptr &&
                 find (pps->begin (), pps->end (), dp->name) == pps->end ()))
              break;

            // See the package_prerequisites definition for details on
            // creating the map keys with the database passed.
            //
            prerequisites.emplace_back (
              lazy_shared_ptr<selected_package> (pdb, dp),
              prerequisite_info {*dc});
          }

          // Try the next alternative if there are unresolved dependencies for
          // this alternative.
          //
          if (i != e)
            continue;

          // Now add the selected packages resolved for the alternative into
          // the dependent's prerequisites map and skip the remaining
          // alternatives.
          //
          for (auto& pr: prerequisites)
          {
            const package_name& pn (pr.first.object_id ());
            const prerequisite_info& pi (pr.second);

            auto p (prereqs.emplace (pr.first, pi));

            // Currently we can only capture a single constraint, so if we
            // already have a dependency on this package and one constraint is
            // not a subset of the other, complain.
            //
            if (!p.second)
            {
              auto& c1 (p.first->second.constraint);
              auto& c2 (pi.constraint);

              bool s1 (satisfies (c1, c2));
              bool s2 (satisfies (c2, c1));

              if (!s1 && !s2)
                fail << "multiple dependencies on package " << pn <<
                  info << pn << " " << *c1 <<
                  info << pn << " " << *c2;

              if (s2 && !s1)
                c1 = c2;
            }

            // If the prerequisite is configured in the linked configuration,
            // then add the respective config.import.* variable.
            //
            if (!simulate)
            {
              database& pdb (pr.first.database ());

              if (pdb != db)
              {
                shared_ptr<selected_package> sp (pr.first.load ());

                optional<pair<package_state, package_substate>> ps;
                if (fps != nullptr)
                  ps = fps (sp);

                if (ps
                    ? ps->second != package_substate::system
                    : !sp->system ())
                {
                  // @@ Note that this doesn't work for build2 modules that
                  //    require bootstrap. For their dependents we need to
                  //    specify the import variable as a global override,
                  //    whenever required (configure, update, etc).
                  //
                  //    This, in particular, means that if we build a package
                  //    that doesn't have direct build2 module dependencies
                  //    but some of its (potentially indirect) dependencies
                  //    do, then we still need to specify the !config.import.*
                  //    global overrides for all of the involved build2
                  //    modules. Implementation of that feels too hairy at the
                  //    moment, so let's handle all the build2 modules
                  //    uniformly for now.
                  //
                  //    Also note that such modules are marked with `requires:
                  //    bootstrap` in their manifest.
                  //
                  //    Note that we currently don't support global overrides
                  //    in the shared build2 context (but could probably do,
                  //    if necessary).
                  //

                  dir_path od;
                  if (ps)
                  {
                    // There is no out_root for a would-be configured package.
                    // So we calculate it like in pkg_configure() below (yeah,
                    // it's an ugly hack).
                    //
                    od = sp->external ()
                      ? pdb.config / dir_path (sp->name.string ())
                      : pdb.config / dir_path (sp->name.string () + '-' +
                                               sp->version.string ());
                  }
                  else
                    od = sp->effective_out_root (pdb.config);

                  // We tried to use global overrides to recreate the original
                  // behavior of not warning about unused config.import.*
                  // variables (achived via the config.config.persist value in
                  // amalgamation). Even though it's probably misguided (we
                  // don't actually save the unused values anywhere, just
                  // don't warn about them).
                  //
                  // Can we somehow cause a clash, say if the same package
                  // comes from different configurations? Yeah, we probably
                  // can. So could add it as undermined (?), detect a clash,
                  // and "fallforward" to the correct behavior.
                  //
                  // But we can clash with an absent value -- that is, we
                  // force importing from a wrong configuration where without
                  // any import things would have been found in the same
                  // amalgamation. Maybe we could detect that (no import
                  // for the same package -- but it could be for a package
                  // we are not configuring).
                  //
                  vars.push_back ("config.import." + sp->name.variable () +
                                  "='" + od.representation () + '\'');
                }
              }
            }
          }

          selected_alt = &eda;
          break;
        }

        // Fail if no dependency alternative is selected, unless we are in the
        // "recreate dependency decisions" mode. In the latter case fall back
        // to the "make dependency decisions" mode and retry.
        //
        if (selected_alt == nullptr)
        {
          if (pps != nullptr)
          {
            pps = nullptr;
            continue;
          }

          fail << "unable to satisfy dependency on " << das;
        }

        const dependency_alternative& da (selected_alt->first);

        // In the reconfiguration mode ban the usage of the selected
        // alternative dependency configuration variables in the subsequent
        // enable and reflect clauses, unless we are also unconstraining
        // dependencies (which indicates it's a relaxed mode that precedes
        // a drop or failure with better diagnostics).
        //
        if (alts == nullptr && !manual  &&
            unconstrain_deps == nullptr &&
            (da.prefer || da.require))
        {
          for (const dependency& d: da)
            banned_var_prefixes.push_back (
              "config." + d.name.variable () + '.');
        }

        // Evaluate the selected dependency alternative reflect clause, if
        // present, unless it refers to any banned variables in which case we
        // fail.
        //
        if (da.reflect)
        {
          if (!banned_var_prefixes.empty ())
            verify_banned_vars (*da.reflect, "reflect");

          ps.evaluate_reflect (*da.reflect,
                               make_pair (di, selected_alt->second));
        }

        dep_alts.push_back (selected_alt->second + 1);

        // The dependency alternative is selected and its dependencies are
        // resolved to the selected packages. So proceed to the next depends
        // value.
        //
        break;
      }
    }

    // Make sure we didn't miss any selected dependency alternative.
    //
    assert (dep_alts.size () == deps.size ());

    // Add the rest of the configuration variables (user overrides, reflects,
    // etc) as well as their sources.
    //
    vector<config_variable> srcs;
    string checksum;

    if (!simulate)
    {
      checksum = ps.config_checksum ();

      pair<strings, vector<config_variable>> rvs (move (ps).collect_config ());

      strings& vs (rvs.first);
      srcs = move (rvs.second);

      if (!vs.empty ())
      {
        if (vars.empty ())
          vars = move (vs);
        else
        {
          vars.reserve (vars.size () + vs.size ());

          for (string& v: vs)
            vars.push_back (move (v));
        }
      }
    }

    return configure_prerequisites_result {move (prereqs),
                                           move (dep_alts),
                                           move (vars),
                                           move (srcs),
                                           move (checksum)};
  }


  unique_ptr<build2::context>
  pkg_configure_context (
    const common_options& o,
    strings&& cmd_vars,
    const function<build2::context::var_override_function>& var_ovr_func)
  {
    using namespace build2;

    // Initialize the build system.
    //
    // Note that this takes into account --build-option and default options
    // files (which may have global overrides and which end up in
    // build2_cmd_vars).
    //
    if (!build2_sched.started ())
      build2_init (o);

    // Re-tune the scheduler for parallel execution (see build2_init()
    // for details).
    //
    if (build2_sched.tuned ())
      build2_sched.tune (0);

    auto merge_cmd_vars = [&cmd_vars] () -> const strings&
    {
      if (cmd_vars.empty ())
        return build2_cmd_vars;

      if (!build2_cmd_vars.empty ())
        cmd_vars.insert (cmd_vars.begin (),
                         build2_cmd_vars.begin (), build2_cmd_vars.end ());

      return cmd_vars;
    };

    // Shouldn't we shared the module context with package skeleton
    // contexts? Maybe we don't have to since we don't build modules in
    // them concurrently (in a sence, we didn't share it when we were
    // invoking the build system driver).
    //
    unique_ptr<context> ctx (
      new context (build2_sched,
                   build2_mutexes,
                   build2_fcache,
                   nullopt /* match_only */,
                   false   /* no_external_modules */,
                   false   /* dry_run */,
                   false   /* no_diag_buffer */,
                   false   /* keep_going */,
                   merge_cmd_vars (),
                   context::reserves {
                     30000 /* targets */,
                     1100  /* variables */},
                   nullptr /* module_context */,
                   nullptr /* inherited_mudules_lock */,
                   var_ovr_func));

    // Set the current meta-operation once per context so that we don't reset
    // ctx->current_on. Note that this function also sets ctx->current_mname
    // and var_build_meta_operation on global scope.
    //
    ctx->current_meta_operation (config::mo_configure);
    ctx->current_oname = string (); // default

    return ctx;
  }

  void
  pkg_configure (const common_options& o,
                 database& db,
                 transaction& t,
                 const shared_ptr<selected_package>& p,
                 configure_prerequisites_result&& cpr,
#ifndef BPKG_OUTPROC_CONFIGURE
                 const unique_ptr<build2::context>& pctx,
                 const build2::variable_overrides& ovrs,
#else
                 const unique_ptr<build2::context>&,
                 const build2::variable_overrides&, // Still in cpr.config_variables.
#endif
                 bool simulate)
  {
    tracer trace ("pkg_configure");

    assert (p->state == package_state::unpacked);
    assert (p->src_root); // Must be set since unpacked.

    tracer_guard tg (db, trace);

#ifndef BPKG_OUTPROC_CONFIGURE
    const dir_path& c (db.config); // Absolute.
#else
    const dir_path& c (db.config_orig); // Relative.
#endif

    dir_path src_root (p->effective_src_root (c));

    // Calculate package's out_root.
    //
    // Note: see a version of this in pkg_configure_prerequisites().
    //
    dir_path out_root (
      p->external ()
      ? c / dir_path (p->name.string ())
      : c / dir_path (p->name.string () + '-' + p->version.string ()));

    l4 ([&]{trace << "src_root: " << src_root << ", "
                  << "out_root: " << out_root;});

    assert (p->prerequisites.empty () && p->dependency_alternatives.empty ());

    p->prerequisites = move (cpr.prerequisites);
    p->dependency_alternatives = move (cpr.dependency_alternatives);

    // Mark the section as loaded, so dependency alternatives are updated.
    //
    p->dependency_alternatives_section.load ();

    // Configure.
    //
    if (!simulate)
    {
      // Original implementation that runs the standard build system driver.
      //
      // Note that the semantics doesn't match 100%. In particular, in the
      // in-process implementation we enter overrides with global visibility
      // in each project instead of the amalgamation (which is probably more
      // accurate, since we don't re-configure the amalgamation nor some
      // dependencies which could be affected by such overrides). In a sense,
      // we enter them as if they were specified with the special .../ scope
      // (but not with the % project visibility -- they must still be visible
      // in subprojects).
      //
#ifdef BPKG_OUTPROC_CONFIGURE
      // Form the buildspec.
      //
      string bspec;

      // Use path representation to get canonical trailing slash.
      //
      if (src_root == out_root)
        bspec = "configure('" + out_root.representation () + "')";
      else
        bspec = "configure('" +
          src_root.representation () + "'@'" +
          out_root.representation () + "')";

      l4 ([&]{trace << "buildspec: " << bspec;});

      try
      {
        run_b (o, verb_b::quiet, cpr.config_variables, bspec);
      }
      catch (const failed&)
      {
        // See below for comments.
        //
        p->out_root = out_root.leaf ();
        p->state = package_state::broken;
        pkg_disfigure (o, db, t, p, true, true, false);
        throw;
      }
#else
      // Print the out-process command line in the verbose mode.
      //
      if (verb >= 2)
      {
        string bspec;

        // Use path representation to get canonical trailing slash.
        //
        if (src_root == out_root)
          bspec = "configure('" + out_root.representation () + "')";
        else
          bspec = "configure('" +
            src_root.representation () + "'@'" +
            out_root.representation () + "')";

        print_b (o, verb_b::quiet, cpr.config_variables, bspec);
      }

      try
      {
        // Note: no bpkg::failed should be thrown from this block.
        //
        using namespace build2;
        using build2::fail;
        using build2::info;
        using build2::endf;
        using build2::location;

        // The build2_init() function initializes the build system verbosity
        // as if running with verb_b::normal while we need verb_b::quiet. So
        // we temporarily adjust the build2 verbosity (see map_verb_b() for
        // details).
        //
        auto verbg (make_guard ([ov = build2::verb] () {build2::verb = ov;}));
        if (bpkg::verb == 1)
          build2::verb = 0;

        context& ctx (*pctx);

        // Bootstrap and load the project.
        //
        // Note: in many ways similar to package_skeleton code.
        //
        scope& rs (*create_root (ctx, out_root, src_root)->second.front ());

        // If we are configuring in the dependency order (as we should), then
        // it feels like the only situation where we can end up with an
        // already bootstrapped project is an unspecified dependency. Note
        // that this is a hard fail since it would have been loaded without
        // the proper configuration.
        //
        if (bootstrapped (rs))
        {
          fail << p->name << db << " loaded ahead of its dependents" <<
            info << "likely unspecified dependency on package " << p->name;
        }

        optional<bool> altn;
        value& v (bootstrap_out (rs, altn));

        if (!v)
          v = src_root;
        else
        {
          dir_path& p (cast<dir_path> (v));

          if (src_root != p)
          {
            // @@ Fuzzy if need this or can do as package skeleton (seeing
            //    that we know we are re-configuring).
            //
            ctx.new_src_root = src_root;
            ctx.old_src_root = move (p);
            p = src_root;
          }
        }

        setup_root (rs, false /* forwarded */);

        // Note: we already know our amalgamation.
        //
        bootstrap_pre (rs, altn);
        bootstrap_src (rs, altn,
                       c.relative (out_root) /* amalgamation */,
                       true                  /* subprojects */);

        create_bootstrap_outer (rs, true /* subprojects */);
        bootstrap_post (rs);

        values mparams;
        const meta_operation_info& mif (config::mo_configure);
        const operation_info& oif (op_default);

        // Skip configure_pre() and configure_operation_pre() calls since we
        // don't pass any parameteres and pass default operation. We also know
        // that op_default has no pre/post operations, naturally.

        // Find the root buildfile. Note that the implied buildfile logic does
        // not apply (our target is the project root directory).
        //
        optional<path> bf (find_buildfile (src_root, src_root, altn));

        if (!bf)
          fail << "no buildfile in " << src_root;

        // Enter project-wide overrides.
        //
        // Note that the use of the root scope as amalgamation makes sure
        // scenarious like below work correctly (see above for background).
        //
        // bpkg create -d cfg cc config.cc.coptions=-Wall
        // bpkg build { config.cc.coptions+=-g }+ libfoo
        //            { config.cc.coptions+=-O }+ libbar
        //
        ctx.enter_project_overrides (rs, out_root, ovrs, &rs);

        // The goal here is to be more or less semantically equivalent to
        // configuring several projects at once. Except that here we have
        // interleaving load/match instead of first all load then all
        // match. But presumably this shouldn't be a problem (we can already
        // have match interrupted by load and the "island append" requirement
        // should hold here as well).
        //
        // Note that either way we will be potentially re-matching the same
        // dependency targets multiple times (see build2::configure_execute()
        // for details).
        //
        const path_name bsn ("<buildspec>");
        const location loc (bsn, 0, 0);

        // out_root/dir{./}
        //
        target_key tk {
          &dir::static_type,
          &out_root,
          &empty_dir_path,
          &empty_string,
          nullopt};

        action_targets tgs;
        mif.load (mparams, rs, *bf, out_root, src_root, loc);
        mif.search (mparams, rs, rs, *bf, tk, loc, tgs);

        ctx.current_operation (oif, nullptr);
        action a (ctx.current_action ());

        mif.match   (mparams, a, tgs, 2 /* diag */, true /* progress */);
        mif.execute (mparams, a, tgs, 2 /* diag */, true /* progress */);

        // Note: no operation_post/meta_operation_post for configure.

        // Here is a tricky part: if this is a normal package, then it will be
        // discovered as a subproject of the bpkg configuration when we load
        // it for the first time (because they are all unpacked). However, if
        // this is a package with src_root!=out_root (such as an external
        // package or a package with a custom checkout_root) then there could
        // be no out_root directory for it in the bpkg configuration yet. As a
        // result, we need to manually add it as a newly discovered
        // subproject.
        //
        if (!rs.out_eq_src ())
        {
          scope* as (rs.parent_scope ()->root_scope ());
          assert (as != nullptr); // No bpkg configuration?

          // Kept NULL if there are no subprojects, so we may need to
          // initialize it (see build2::bootstrap_src() for details).
          //
          subprojects* sp (*as->root_extra->subprojects);
          if (sp == nullptr)
          {
            value& v (as->vars.assign (*ctx.var_subprojects));
            v = subprojects {};
            sp = *(as->root_extra->subprojects = &cast<subprojects> (v));
          }

          const project_name& n (**rs.root_extra->project);

          if (sp->find (n) == sp->end ())
            sp->emplace (n, out_root.leaf ());
        }
      }
      catch (const build2::failed&)
      {
        // Assume the diagnostics has already been issued.

        // If we failed to configure the package, make sure we revert
        // it back to the unpacked state by running disfigure (it is
        // valid to run disfigure on an un-configured build). And if
        // disfigure fails as well, then the package will be set into
        // the broken state.

        // Indicate to pkg_disfigure() we are partially configured.
        //
        p->out_root = out_root.leaf ();
        p->state = package_state::broken;

        // Commits the transaction.
        //
        pkg_disfigure (o, db, t,
                       p,
                       true /* clean */,
                       true /* disfigure */,
                       false /* simulate */);


        throw bpkg::failed ();
      }
#endif

      p->config_variables = move (cpr.config_sources);
      p->config_checksum  = move (cpr.config_checksum);
    }

    p->out_root = out_root.leaf ();
    p->state = package_state::configured;

    db.update (p);
    t.commit ();
  }

  void
  pkg_configure (const common_options& o,
                 database& db,
                 transaction& t,
                 const shared_ptr<selected_package>& p,
                 const dependencies& deps,
                 const vector<size_t>* alts,
                 package_skeleton&& ps,
                 const vector<package_name>* pps,
                 bool disfigured,
                 bool simulate,
                 const function<find_database_function>& fdb)
  {
    configure_prerequisites_result cpr (
      pkg_configure_prerequisites (o,
                                   db,
                                   t,
                                   deps,
                                   alts,
                                   move (ps),
                                   pps,
                                   simulate,
                                   fdb,
                                   nullptr));

    if (!simulate)
    {
      // Unless this package has been completely disfigured, disfigure all the
      // package configuration variables to reset all the old values to
      // defaults (all the new user/dependent/reflec values, including old
      // user, are returned by collect_config() and specified as overrides).
      // Note that this semantics must be consistent with how we load things
      // in the package skeleton during configuration negotiation.
      //
      // Note also that this means we don't really use the dependent and
      // reflect sources that we save in the database. But let's keep them
      // for the completeness of information (maybe could be useful during
      // configuration reset or some such).
      //
      if (!disfigured)
      {
        // Note: must be quoted to preserve the pattern.
        //
        cpr.config_variables.push_back (
          "config.config.disfigure='config." + p->name.variable () + "**'");
      }
    }

    unique_ptr<build2::context> ctx;

#ifndef BPKG_OUTPROC_CONFIGURE
    if (!simulate)
      ctx = pkg_configure_context (o, move (cpr.config_variables));
#endif

    pkg_configure (o,
                   db,
                   t,
                   p,
                   move (cpr),
                   ctx,
                   (ctx != nullptr
                    ? ctx->var_overrides
                    : build2::variable_overrides {}),
                   simulate);
  }

  shared_ptr<selected_package>
  pkg_configure_system (const package_name& n,
                        const version& v,
                        database& db,
                        transaction& t)
  {
    tracer trace ("pkg_configure_system");

    tracer_guard tg (db, trace);

    shared_ptr<selected_package> p (
      new selected_package {
        n,
        v,
        package_state::configured,
        package_substate::system,
        false,                     // Don't hold package.
        false,                     // Don't hold version.
        repository_location (),    // Root repository fragment.
        nullopt,                   // No source archive.
        false,                     // No auto-purge (does not get there).
        nullopt,                   // No source directory.
        false,
        nullopt,                   // No manifest checksum.
        nullopt,                   // No buildfiles checksum.
        nullopt,                   // No output directory.
        {}});                      // No prerequisites.

    db.persist (p);
    t.commit ();

    return p;
  }

  int
  pkg_configure (const pkg_configure_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_configure");

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    // Sort arguments into the package name and configuration variables.
    //
    string n;
    strings vars;
    bool sep (false); // Seen '--'.

    while (args.more ())
    {
      string a (args.next ());

      // If we see the "--" separator, then we are done parsing variables.
      //
      if (!sep && a == "--")
      {
        sep = true;
        continue;
      }

      if (!sep && a.find ('=') != string::npos)
        vars.push_back (move (trim (a)));
      else if (n.empty ())
        n = move (a);
      else
        fail << "unexpected argument '" << a << "'";
    }

    if (n.empty ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-configure' for more information";

    const char* package (n.c_str ());
    package_scheme ps (parse_package_scheme (package));

    if (ps == package_scheme::sys && !vars.empty ())
      fail << "configuration variables specified for a system package";

    database db (c, trace, true /* pre_attach */);
    transaction t (db);
    session s;

    shared_ptr<selected_package> p;

    // pkg_configure() commits the transaction.
    //
    if (ps == package_scheme::sys)
    {
      // Configure system package.
      //
      version v (parse_package_version (package));
      package_name n (parse_package_name (package));

      p = db.find<selected_package> (n);

      if (p != nullptr)
        fail << "package " << n << " already exists in configuration " << c;

      shared_ptr<repository_fragment> root (db.load<repository_fragment> (""));

      using query = query<available_package>;
      query q (query::id.name == n);

      if (filter_one (root, db.query<available_package> (q)).first == nullptr)
        fail << "unknown package " << n;

      p = pkg_configure_system (n, v.empty () ? wildcard_version : v, db, t);
    }
    else
    {
      // Configure unpacked package.
      //
      p = db.find<selected_package> (
        parse_package_name (n, false /* allow_version */));

      if (p == nullptr)
        fail << "package " << n << " does not exist in configuration " << c;

      if (p->state != package_state::unpacked)
        fail << "package " << n << " is " << p->state <<
          info << "expected it to be unpacked";

      l4 ([&]{trace << *p;});

      // Let's not bother trying to find an available package for this
      // selected package, which may potentially not be present in this
      // configuration (but instead be present in the configuration we are
      // linked to, etc) and create a transient available package outright.
      //
      shared_ptr<available_package> ap (make_available (o, db, p));

      optional<dir_path> src_root (p->external () ? p->src_root : nullopt);

      optional<dir_path> out_root (src_root
                                   ? dir_path (db.config) /= p->name.string ()
                                   : optional<dir_path> ());

      // Note on the disfigure logic: while we don't know whether the package
      // has been disfigured with --keep-config or not, it has already been
      // done physically and if without --keep-config, then config.build has
      // been removed and config_variables cleaned. As a result, we can just
      // proceed as disfigure=false and disfigure=true will be taken care
      // automatically (because then things have been removed/cleaned).
      //
      pkg_configure (o,
                     db,
                     t,
                     p,
                     ap->dependencies,
                     nullptr /* alternatives */,
                     package_skeleton (o,
                                       package_key (db, ap->id.name),
                                       false /* system */,
                                       ap,
                                       move (vars),
                                       false /* disfigure */,
                                       &p->config_variables,
                                       move (src_root),
                                       move (out_root),
                                       nullopt /* old_src_root */,
                                       nullopt /* old_out_root */,
                                       package_skeleton::load_config_user |
                                       package_skeleton::load_config_dependent),
                     nullptr /* prerequisites */,
                     false /* disfigured */,
                     false /* simulate */);
    }

    if (verb && !o.no_result ())
      text << "configured " << *p;

    return 0;
  }
}
