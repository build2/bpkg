// file      : bpkg/pkg-bindist.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-bindist.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/package-query.hxx>
#include <bpkg/database.hxx>
#include <bpkg/pkg-verify.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/system-package-manager.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  using packages = system_package_manager::packages;
  using recursive_mode = system_package_manager::recursive_mode;

  // Find the available package(s) for the specified selected package.
  //
  // Specifically, for non-system packages we look for a single available
  // package failing if it's an orphan. For system packages we look for all
  // the available packages analogous to pkg-build. If none are found then
  // we assume the --sys-no-stub option was used to configure this package
  // and return an empty list. @@ What if it was configured with a specific
  // bpkg version or `*`?
  //
  static available_packages
  find_available_packages (const common_options& co,
                           database& db,
                           const shared_ptr<selected_package>& p)
  {
    assert (p->state == package_state::configured);

    available_packages r;
    if (p->substate == package_substate::system)
    {
      r = find_available_all (repo_configs, p->name);
    }
    else
    {
      pair<shared_ptr<available_package>,
           lazy_shared_ptr<repository_fragment>> ap (
             find_available_fragment (co, db, p));

      if (ap.second.loaded () && ap.second == nullptr)
        fail << "package " << p->name << " is orphaned";

      r.push_back (move (ap));
    }

    return r;
  }

  // Collect dependencies of the specified package, potentially recursively.
  // System dependencies go to deps, non-system -- to pkgs, which could be the
  // same as deps or NULL, depending on the desired semantics (see the call
  // site for details). Find available packages for deps.
  //
  static void
  collect_dependencies (const common_options& co,
                        packages* pkgs,
                        packages& deps,
                        const selected_package& p,
                        bool recursive)
  {
    for (const auto& pr: p.prerequisites)
    {
      const lazy_shared_ptr<selected_package>& ld (pr.first);

      // We only consider dependencies from target configurations, similar
      // to pkg-install.
      //
      database& db (ld.database ());
      if (db.type == host_config_type || db.type == build2_config_type)
        continue;

      shared_ptr<selected_package> d (ld.load ());

      // The selected package can only be configured if all its dependencies
      // are configured.
      //
      assert (d->state == package_state::configured);

      bool sys (d->substate == package_substate::system);
      packages* ps (sys ? &deps : pkgs);

      // Skip duplicates.
      //
      if (ps == nullptr ||
          find_if (ps->begin (), ps->end (),
                   [&d] (const pair<shared_ptr<selected_package>,
                                    available_packages>& p)
                   {
                     return p.first == d;
                   }) == ps->end ())
      {
        available_packages aps;
        if (ps == &deps) // Note: covers the (pkgs == &deps) case.
          aps = find_available_packages (co, db, d);

        const selected_package& p (*d);

        if (ps != nullptr)
          ps->push_back (make_pair (move (d), move (aps)));

        if (recursive && !sys)
          collect_dependencies (co, pkgs, deps, p, recursive);
      }
    }
  }

  int
  pkg_bindist (const pkg_bindist_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_bindist");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    // Verify options.
    //
    optional<recursive_mode> rec;
    {
      diag_record dr;

      if (o.recursive_specified ())
      {
        const string& m (o.recursive ());

        if      (m == "auto") rec = recursive_mode::auto_;
        else if (m == "full") rec = recursive_mode::full;
        else
          dr << fail << "unknown mode '" << m << "' specified with --recursive";
      }
      else if (o.private_ ())
        dr << fail << "--private specified without --recursive";

      if (!dr.empty ())
        dr << info << "run 'bpkg help pkg-bindist' for more information";
    }

    // Sort arguments into the output directory, package names, and
    // configuration variables.
    //
    dir_path out;
    vector<package_name> pns;
    strings vars;
    {
      bool sep (false); // Seen `--`.

      while (args.more ())
      {
        string a (args.next ());

        // If we see the `--` separator, then we are done parsing variables
        // (while they won't clash with package names, we may be given a
        // directory path that contains `=`).
        //
        if (!sep && a == "--")
        {
          sep = true;
          continue;
        }

        if (a.find ('=') != string::npos)
          vars.push_back (move (trim (a)));
        else if (out.empty ())
        {
          try
          {
            out = dir_path (move (a));
          }
          catch (const invalid_path& e)
          {
            fail << "invalid output directory '" << e.path << "'";
          }
        }
        else
        {
          try
          {
            pns.push_back (package_name (move (a))); // Not moved on failure.
          }
          catch (const invalid_argument& e)
          {
            fail << "invalid package name '" << a << "': " << e;
          }
        }
      }

      if (out.empty () || pns.empty ())
        fail << "output directory or package name argument expected" <<
          info << "run 'bpkg help pkg-bindist' for more information";
    }

    database db (c, trace, true /* pre_attach */);

    // Similar to pkg-install we disallow generating packages from the
    // host/build2 configurations.
    //
    if (db.type == host_config_type || db.type == build2_config_type)
    {
      fail << "unable to generate distribution package from " << db.type
           << " configuration" <<
        info << "use target configuration instead";
    }

    // Prepare for the find_available_*() calls.
    //
    repo_configs.push_back (db);

    transaction t (db);

    // We need to suppress duplicate dependencies for the recursive mode.
    //
    session ses;

    // Resolve package names to selected packages and verify they are all
    // configured. While at it collect their available packages and
    // dependencies.
    //
    packages pkgs, deps;

    for (const package_name& n: pns)
    {
      shared_ptr<selected_package> p (db.find<selected_package> (n));

      if (p == nullptr)
        fail << "package " << n << " does not exist in configuration " << c;

      if (p->state != package_state::configured)
        fail << "package " << n << " is " << p->state <<
          info << "expected it to be configured";

      if (p->substate == package_substate::system)
        fail << "package " << n << " is configured as system";

      // If this is the first package, load its available package for the
      // mapping information. We don't need it for any additional packages.
      //
      available_packages aps;
      if (pkgs.empty ())
        aps = find_available_packages (o, db, p);

      const selected_package& r (*p);
      pkgs.push_back (make_pair (move (p), move (aps)));

      // If --recursive is not specified then we want all the immediate
      // (system and non-) dependecies in deps. Otherwise, if the recursive
      // mode is full, then we want all the transitive non-system dependecies
      // in pkgs. In both recursive modes we also want all the transitive
      // system dependecies in deps.
      //
      // Note also that in the auto recursive mode it's possible that some of
      // the system dependencies are not really needed. But there is no way
      // for us to detect this and it's better to over- than under-specify.
      //
      collect_dependencies (o,
                            (rec
                             ? *rec == recursive_mode::full ? &pkgs : nullptr
                             : &deps),
                            deps,
                            r,
                            rec.has_value ());
    }

    t.commit ();

    // Load the package manifest (source of extra metadata). This should be
    // always possible since the package is configured and is not system.
    //
    const shared_ptr<selected_package>& sp (pkgs.front ().first);

    package_manifest pm (
      pkg_verify (o,
                  sp->effective_src_root (db.config_orig),
                  true  /* ignore_unknown */,
                  false /* ignore_toolchain */,
                  false /* load_buildfiles */,
                  // Copy potentially fixed up version from selected package.
                  [&sp] (version& v) {v = sp->version;}));

    // Note that we shouldn't need to install anything or use sudo.
    //
    unique_ptr<system_package_manager> spm (
      make_production_system_package_manager (o,
                                              host_triplet,
                                              o.distribution (),
                                              o.architecture ()));
    if (spm == nullptr)
    {
      fail << "no standard distribution package manager for this host "
           << "or it is not yet supported" <<
        info << "consider specifying alternative distribution package "
           << "manager with --distribution";
    }

    // @@ TODO: pass/handle --private.

    spm->generate (pkgs, deps, vars, pm, out, rec);

    // @@ TODO: change the output, maybe to something returned by spm?
    //
    if (verb && !o.no_result ())
    {
      const selected_package& p (*pkgs.front ().first);

      text << "generated " << spm->os_release.name_id << " package for "
           << p.name << '/' << p.version;
    }

    return 0;
  }

  pkg_bindist_options
  merge_options (const default_options<pkg_bindist_options>& defs,
                 const pkg_bindist_options& cmd)
  {
    // NOTE: remember to update the documentation if changing anything here.

    return merge_default_options (
      defs,
      cmd,
      [] (const default_options_entry<pkg_bindist_options>& e,
          const pkg_bindist_options&)
      {
        const pkg_bindist_options& o (e.options);

        auto forbid = [&e] (const char* opt, bool specified)
        {
          if (specified)
            fail (e.file) << opt << " in default options file";
        };

        forbid ("--directory|-d", o.directory_specified ());
      });
  }
}
