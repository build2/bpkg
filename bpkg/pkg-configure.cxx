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
  package_prerequisites
  pkg_configure_prerequisites (const common_options& o,
                               database& db,
                               transaction&,
                               const dependencies& deps,
                               const package_name& package,
                               const function<find_database_function>& fdb)
  {
    package_prerequisites r;

    for (const dependency_alternatives_ex& das: deps)
    {
      // @@ DEP Currently we just pick the first alternative with dependencies
      //    that can all be resolved to the configured packages, satisfying
      //    the respective constraints. Later, we should also evaluate the
      //    alternative enable conditions.
      //
      assert (!das.conditional ());

      bool satisfied (false);
      for (const dependency_alternative& da: das)
      {
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

          if (das.buildtime)
          {
            // Handle special names.
            //
            if (n == "build2")
            {
              if (d.constraint && !satisfy_build2 (o, d))
                fail << "unable to satisfy constraint (" << d
                     << ") for package " << package <<
                  info << "available build2 version is " << build2_version;

              continue;
            }
            else if (n == "bpkg")
            {
              if (d.constraint && !satisfy_bpkg (o, d))
                fail << "unable to satisfy constraint (" << d
                     << ") for package " << package <<
                  info << "available bpkg version is " << bpkg_version;

              continue;
            }
          }

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

          auto p (r.emplace (pr.first, pc));

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
        }

        satisfied = true;
        break;
      }

      if (!satisfied)
        fail << "unable to satisfy dependency on " << das;
    }

    return r;
  }

  void
  pkg_configure (const common_options& o,
                 database& db,
                 transaction& t,
                 const shared_ptr<selected_package>& p,
                 const dependencies& deps,
                 const strings& vars,
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

    p->prerequisites = pkg_configure_prerequisites (o,
                                                    db,
                                                    t,
                                                    deps,
                                                    p->name,
                                                    fdb);

    if (!simulate)
    {
      // Add the config.import.* variables for prerequisites from the linked
      // configurations.
      //
      strings imports;

      for (const auto& pp: p->prerequisites)
      {
        database& pdb (pp.first.database ());

        if (pdb != db)
        {
          shared_ptr<selected_package> sp (pp.first.load ());

          if (!sp->system ())
          {
            // @@ Note that this doesn't work for build2 modules that require
            //    bootstrap. For their dependents we need to specify the
            //    import variable as a global override, whenever required
            //    (configure, update, etc).
            //
            //    This, in particular, means that if we build a package that
            //    doesn't have direct build2 module dependencies but some of
            //    its (potentially indirect) dependencies do, then we still
            //    need to specify the !config.import.* global overrides for
            //    all of the involved build2 modules. Implementation of that
            //    feels too hairy at the moment, so let's handle all the
            //    build2 modules uniformly for now.
            //
            //    Also note that such modules are marked with `requires:
            //    bootstrap` in their manifest.
            //
            dir_path od (sp->effective_out_root (pdb.config));
            imports.push_back ("config.import." + sp->name.variable () +
                               "='" + od.representation () + "'");
          }
        }
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

      // Configure.
      //
      try
      {
        run_b (o, verb_b::quiet, imports, vars, bspec);
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
        vars.push_back (move (a));
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

      package_manifest m (pkg_verify (o,
                                      p->effective_src_root (c),
                                      true /* ignore_unknown */,
                                      [&p] (version& v) {v = p->version;}));

      pkg_configure (o,
                     db,
                     t,
                     p,
                     convert (move (m.dependencies)),
                     vars,
                     false /* simulate */);
    }

    if (verb && !o.no_result ())
      text << "configured " << *p;

    return 0;
  }
}
