// file      : bpkg/pkg-configure.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-configure.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/satisfaction.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/pkg-verify.hxx>
#include <bpkg/pkg-disfigure.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  // Given dependencies of a package, return its prerequisite packages,
  // configuration variables that resulted from selection of these
  // prerequisites (import, reflection, etc), and sources of the configuration
  // variables resulted from evaluating the reflect clauses. See
  // pkg_configure() for the semantics of the dependency list. Fail if for
  // some of the dependency alternative lists there is no satisfactory
  // alternative (all its dependencies are configured, satisfy the respective
  // constraints, etc).
  //
  struct configure_prerequisites_result
  {
    package_prerequisites   prerequisites;
    strings                 config_variables; // Note: name and value.

    // Only contains sources of configuration variables collected using the
    // package skeleton, excluding those user-specified variables which are
    // not the project variables for the specified package (module
    // configuration variables, etc). Thus, it is not parallel to the
    // variables member.
    //
    vector<config_variable> config_sources; // Note: name and source.
  };

  // Note: loads selected packages.
  //
  static configure_prerequisites_result
  pkg_configure_prerequisites (const common_options& o,
                               database& db,
                               transaction&,
                               const dependencies& deps,
                               package_skeleton&& ps,
                               bool simulate,
                               const function<find_database_function>& fdb)
  {
    package_prerequisites   prereqs;
    strings                 vars;
    vector<config_variable> srcs;

    for (size_t di (0); di != deps.size (); ++di)
    {
      const dependency_alternatives_ex& das (deps[di]);

      if (das.empty () || toolchain_buildtime_dependency (o, das, ps.name ()))
        continue;

      // Pick the first alternative with dependencies that can all be resolved
      // to the configured packages, satisfying the respective constraints.
      //
      bool satisfied (false);
      bool enabled   (false); // True if there is an enabled alternative.
      for (const dependency_alternative& da: das)
      {
        if (da.enable && !ps.evaluate_enable (*da.enable, di))
          continue;

        enabled = true;

        // Cache the selected packages which correspond to the alternative
        // dependencies, pairing them with the respective constraints. If the
        // alternative turns out to be fully resolvable, we will add the
        // cached packages into the dependent's prerequisites map.
        //
        small_vector<
          pair<lazy_shared_ptr<selected_package>,
               const optional<version_constraint>&>, 1> prerequisites;

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

          if (dp == nullptr                          ||
              dp->state != package_state::configured ||
              !satisfies (dp->version, d.constraint))
            break;

          // See the package_prerequisites definition for details on creating
          // the map keys with the database passed.
          //
          prerequisites.emplace_back (
            lazy_shared_ptr<selected_package> (*spd.second, dp),
            d.constraint);
        }

        // Try the next alternative if there are unresolved dependencies for
        // this alternative.
        //
        if (i != e)
          continue;

        // Now add the selected packages resolved for the alternative into the
        // dependent's prerequisites map and skip the remaining alternatives.
        //
        for (auto& pr: prerequisites)
        {
          const package_name& pn (pr.first.object_id ());
          const optional<version_constraint>& pc (pr.second);

          auto p (prereqs.emplace (pr.first, pc));

          // Currently we can only capture a single constraint, so if we
          // already have a dependency on this package and one constraint is
          // not a subset of the other, complain.
          //
          if (!p.second)
          {
            auto& c (p.first->second);

            bool s1 (satisfies (c, pc));
            bool s2 (satisfies (pc, c));

            if (!s1 && !s2)
              fail << "multiple dependencies on package " << pn <<
                info << pn << " " << *c <<
                info << pn << " " << *pc;

            if (s2 && !s1)
              c = pc;
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

              if (!sp->system ())
              {
                // @@ Note that this doesn't work for build2 modules that
                //    require bootstrap. For their dependents we need to
                //    specify the import variable as a global override,
                //    whenever required (configure, update, etc).
                //
                //    This, in particular, means that if we build a package
                //    that doesn't have direct build2 module dependencies but
                //    some of its (potentially indirect) dependencies do, then
                //    we still need to specify the !config.import.* global
                //    overrides for all of the involved build2
                //    modules. Implementation of that feels too hairy at the
                //    moment, so let's handle all the build2 modules uniformly
                //    for now.
                //
                //    Also note that such modules are marked with `requires:
                //    bootstrap` in their manifest.
                //
                dir_path od (sp->effective_out_root (pdb.config));
                vars.push_back ("config.import." + sp->name.variable () +
                                "='" + od.representation () + "'");
              }
            }
          }
        }

        // Evaluate the dependency alternative reflect clause, if present.
        //
        if (da.reflect)
          ps.evaluate_reflect (*da.reflect, di);

        satisfied = true;
        break;
      }

      if (enabled && !satisfied)
        fail << "unable to satisfy dependency on " << das;
    }

    // Add the configuration variables collected from the reflect clauses, if
    // any.
    //
    if (!simulate)
    {
      auto rvs (move (ps).collect_config ());

      strings&                         vs (rvs.first);
      vector<optional<config_source>>& ss (rvs.second);

      if (!vs.empty ())
      {
        vars.reserve (vars.size () + vs.size ());

        for (size_t i (0); i != vs.size (); ++i)
        {
          string&                        v (vs[i]);
          const optional<config_source>& s (ss[i]);

          if (s)
          {
            size_t p (v.find_first_of ("=+ \t"));
            assert (p != string::npos);

            srcs.push_back (config_variable {string (v, 0, p), *s});
          }

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
                 const dependencies& deps,
                 package_skeleton&& ps,
                 bool simulate,
                 const function<find_database_function>& fdb)
  {
    tracer trace ("pkg_configure");

    assert (p->state == package_state::unpacked);
    assert (p->src_root); // Must be set since unpacked.

    tracer_guard tg (db, trace);

    const dir_path& c (db.config_orig);
    dir_path src_root (p->effective_src_root (c));

    // Calculate package's out_root.
    //
    dir_path out_root (
      p->external ()
      ? c / dir_path (p->name.string ())
      : c / dir_path (p->name.string () + "-" + p->version.string ()));

    l4 ([&]{trace << "src_root: " << src_root << ", "
                  << "out_root: " << out_root;});

    // Verify all our prerequisites are configured and populate the
    // prerequisites list.
    //
    assert (p->prerequisites.empty ());

    configure_prerequisites_result cpr (
      pkg_configure_prerequisites (o,
                                   db,
                                   t,
                                   deps,
                                   move (ps),
                                   simulate,
                                   fdb));

    p->prerequisites = move (cpr.prerequisites);

    if (!simulate)
    {
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

      // Deduce the configuration variables which are not reflected anymore
      // and disfigure them.
      //
      string dvar;
      for (const config_variable& cv: p->config_variables)
      {
        if (cv.source == config_source::reflect)
        {
          const vector<config_variable>& ss (cpr.config_sources);
          auto i (find_if (ss.begin (), ss.end (),
                           [&cv] (const config_variable& v)
                           {
                             return v.name == cv.name;
                           }));

          if (i == ss.end ())
          {
            if (dvar.empty ())
              dvar = "config.config.disfigure=";
            else
              dvar += ' ';

            dvar += cv.name;
          }
        }
      }

      // Configure.
      //
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
        // If we failed to configure the package, make sure we revert
        // it back to the unpacked state by running disfigure (it is
        // valid to run disfigure on an un-configured build). And if
        // disfigure fails as well, then the package will be set into
        // the broken state.
        //

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
        throw;
      }

      p->config_variables = move (cpr.config_sources);
    }

    p->out_root = out_root.leaf ();
    p->state = package_state::configured;

    db.update (p);
    t.commit ();
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

      pkg_configure (o,
                     db,
                     t,
                     p,
                     ap->dependencies,
                     package_skeleton (o,
                                       db,
                                       *ap,
                                       move (vars),
                                       move (src_root),
                                       move (out_root)),
                     false /* simulate */);
    }

    if (verb && !o.no_result ())
      text << "configured " << *p;

    return 0;
  }
}
