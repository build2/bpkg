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
                               transaction& t,
                               const dependencies& deps,
                               const package_name& package)
  {
    package_prerequisites r;

    database& db (t.database ());

    for (const dependency_alternatives_ex& da: deps)
    {
      assert (!da.conditional); //@@ TODO

      bool satisfied (false);
      for (const dependency& d: da)
      {
        const package_name& n (d.name);

        if (da.buildtime)
        {
          // Handle special names.
          //
          if (n == "build2")
          {
            if (d.constraint)
              satisfy_build2 (o, package, d);

            satisfied = true;
            break;
          }
          else if (n == "bpkg")
          {
            if (d.constraint)
              satisfy_bpkg (o, package, d);

            satisfied = true;
            break;
          }
          // else
          //
          // @@ TODO: in the future we would need to at least make sure the
          // build and target machines are the same. See also pkg-build.
        }

        if (shared_ptr<selected_package> dp = db.find<selected_package> (n))
        {
          if (dp->state != package_state::configured)
            continue;

          if (!satisfies (dp->version, d.constraint))
            continue;

          auto p (r.emplace (dp, d.constraint));

          // Currently we can only capture a single constraint, so if we
          // already have a dependency on this package and one constraint is
          // not a subset of the other, complain.
          //
          if (!p.second)
          {
            auto& c (p.first->second);

            bool s1 (satisfies (c, d.constraint));
            bool s2 (satisfies (d.constraint, c));

            if (!s1 && !s2)
              fail << "multiple dependencies on package " << n <<
                info << n << " " << *c <<
                info << n << " " << *d.constraint;

            if (s2 && !s1)
              c = d.constraint;
          }

          satisfied = true;
          break;
        }
      }

      if (!satisfied)
        fail << "no configured package satisfies dependency on " << da;
    }

    return r;
  }

  void
  pkg_configure (const dir_path& c,
                 const common_options& o,
                 transaction& t,
                 const shared_ptr<selected_package>& p,
                 const dependencies& deps,
                 const strings& vars,
                 bool simulate)
  {
    tracer trace ("pkg_configure");

    assert (p->state == package_state::unpacked);
    assert (p->src_root); // Must be set since unpacked.

    database& db (t.database ());
    tracer_guard tg (db, trace);

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

    p->prerequisites = pkg_configure_prerequisites (o, t, deps, p->name);

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

      // Configure.
      //
      try
      {
        run_b (o, verb_b::quiet, vars, bspec);
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
        pkg_disfigure (c, o, t, p, true /* clean */, false /* simulate */);
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
                        transaction& t)
  {
    tracer trace ("pkg_configure_system");

    database& db (t.database ());
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

    database db (open (c, trace));
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

      p = pkg_configure_system (n, v.empty () ? wildcard_version : v, t);
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

      package_manifest m (pkg_verify (p->effective_src_root (c),
                                      true /* ignore_unknown */,
                                      [&p] (version& v) {v = p->version;}));

      pkg_configure (c,
                     o,
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
