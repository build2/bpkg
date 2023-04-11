// file      : bpkg/pkg-configure.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-configure.hxx>

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
                               const function<find_package_state_function>& fps)
  {
    tracer trace ("pkg_configure_prerequisites");

    tracer_guard tg (db, trace);

    package_prerequisites prereqs;
    strings               vars;

    // Alternatives argument must be parallel to the dependencies argument if
    // specified.
    //
    assert (alts == nullptr || alts->size () == deps.size ());

    for (size_t di (0); di != deps.size (); ++di)
    {
      // Skip the toolchain build-time dependencies and dependencies without
      // enabled alternatives.
      //
      const dependency_alternatives_ex& das (deps[di]);

      if (das.empty ())
        continue;

      small_vector<pair<reference_wrapper<const dependency_alternative>,
                        size_t>,
                   2> edas;

      // If the dependency alternatives are not pre-selected, then evaluate
      // the enable clauses.
      //
      // Note that evaluating the require and prefer clauses in this case is
      // meaningless since we don't reconfigure the dependencies nor negotiate
      // configurations with other dependents. What we should probably do is
      // load configurations of the dependencies and use them while evaluating
      // the dependent's enable and reflect clauses as we go along. Probably
      // we should still evaluate the accept clauses to make sure that the
      // dependency is configured acceptably for the dependent. For now we
      // fail and will support this maybe later.
      //
      if (alts == nullptr)
      {
        if (toolchain_buildtime_dependency (o, das, &ps.package.name))
          continue;

        for (size_t i (0); i != das.size (); ++i)
        {
          const dependency_alternative& da (das[i]);

          if (!da.enable || ps.evaluate_enable (*da.enable, make_pair (di, i)))
          {
            if (da.prefer || da.require)
              fail << "manual configuration of dependents with prefer or "
                   << "require clauses is not yet supported";

            edas.push_back (make_pair (ref (da), i));
          }
        }

        if (edas.empty ())
          continue;
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
      for (const vector<package_name>* pps (prev_prereqs);;)
      {
        bool satisfied (false);
        for (const auto& eda: edas)
        {
          const dependency_alternative& da (eda.first);
          size_t dai (eda.second);

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

            optional<pair<package_state, package_substate>> dps;
            if (fps != nullptr)
              dps = fps (dp);

            if ((dps ? dps->first : dp->state) != package_state::configured ||
                !satisfies (dp->version, d.constraint)                      ||
                (pps != nullptr &&
                 find (pps->begin (), pps->end (), dp->name) == pps->end ()))
              break;

            // See the package_prerequisites definition for details on
            // creating the map keys with the database passed.
            //
            bool conf (da.prefer || da.require);

            prerequisites.emplace_back (
              lazy_shared_ptr<selected_package> (*spd.second, dp),
              prerequisite_info {d.constraint,
                                 make_pair (conf ? di  + 1 : 0,
                                            conf ? dai + 1 : 0)});
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

              // Keep position of the first dependency alternative with a
              // configuration clause.
              //
              pair<size_t, size_t>& p1 (p.first->second.config_position);
              pair<size_t, size_t>  p2 (pi.config_position);

              if (p1.first == 0 && p2.first != 0)
                p1 = p2;
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

                  vars.push_back ("config.import." + sp->name.variable () +
                                  "='" + od.representation () + '\'');
                }
              }
            }
          }

          // Evaluate the dependency alternative reflect clause, if present.
          //
          if (da.reflect)
            ps.evaluate_reflect (*da.reflect, make_pair (di, dai));

          satisfied = true;
          break;
        }

        // Fail if no dependency alternative is selected, unless we are in the
        // "recreate dependency decisions" mode. In the latter case fall back
        // to the "make dependency decisions" mode and retry.
        //
        if (!satisfied)
        {
          if (pps != nullptr)
          {
            pps = nullptr;
            continue;
          }

          fail << "unable to satisfy dependency on " << das;
        }

        // The dependency alternative is selected and its dependencies are
        // resolved to the selected packages. So proceed to the next depends
        // value.
        //
        break;
      }
    }

    // Add the rest of the configuration variables (user overrides, reflects,
    // etc) as well as their sources.
    //
    vector<config_variable> srcs;

    if (!simulate)
    {
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
                                           move (vars),
                                           move (srcs)};
  }

  void
  pkg_configure (const common_options& o,
                 database& db,
                 transaction& t,
                 const shared_ptr<selected_package>& p,
                 configure_prerequisites_result&& cpr,
                 bool disfigured,
                 bool simulate)
  {
    tracer trace ("pkg_configure");

    assert (p->state == package_state::unpacked);
    assert (p->src_root); // Must be set since unpacked.

    tracer_guard tg (db, trace);

    const dir_path& c (db.config_orig);
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

    assert (p->prerequisites.empty ());
    p->prerequisites = move (cpr.prerequisites);

    // Configure.
    //
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
      string dvar;
      if (!disfigured)
      {
        // Note: must be quoted to preserve the pattern.
        //
        dvar = "config.config.disfigure='config.";
        dvar += p->name.variable ();
        dvar += "**'";
      }

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
        run_b (o,
               verb_b::quiet,
               cpr.config_variables,
               (!dvar.empty () ? dvar.c_str () : nullptr),
               bspec);
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

      p->config_variables = move (cpr.config_sources);
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

    pkg_configure (o, db, t, p, move (cpr), disfigured, simulate);
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
                                       move (out_root)),
                     nullptr /* prerequisites */,
                     false /* disfigured */,
                     false /* simulate */);
    }

    if (verb && !o.no_result ())
      text << "configured " << *p;

    return 0;
  }
}
