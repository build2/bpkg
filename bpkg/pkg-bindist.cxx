// file      : bpkg/pkg-bindist.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-bindist.hxx>

#include <list>

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
  using package = system_package_manager::package;
  using packages = system_package_manager::packages;

  // Find the available package(s) for the specified selected package.
  //
  // Specifically, for non-system packages we look for a single available
  // package. For system packages we look for all the available packages
  // analogous to pkg-build. If none are found then we assume the
  // --sys-no-stub option was used to configure this package and return an
  // empty list. @@ What if it was configured with a specific bpkg version or
  // `*`?
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
      {
        // This is an orphan. We used to fail but there is no reason we cannot
        // just load its manifest and make an available package out of that.
        // And it's handy to be able to run this command on packages built
        // from archives.
        //
        package_manifest m (
          pkg_verify (co,
                      p->effective_src_root (db.config_orig),
                      true  /* ignore_unknown */,
                      false /* ignore_toolchain */,
                      false /* load_buildfiles */,
                      // Copy potentially fixed up version from selected package.
                      [&p] (version& v) {v = p->version;}));

        // Fake the buildfile information (not used).
        //
        m.alt_naming = false;
        m.bootstrap_build = "project = " + p->name.string () + '\n';

        ap.first = make_shared<available_package> (move (m));

        // Fake the location (only used for diagnostics).
        //
        ap.second = make_shared<repository_fragment> (
          repository_location (
            p->effective_src_root (db.config).representation (),
            repository_type::dir));

        ap.first->locations.push_back (
          package_location {ap.second, current_dir});
      }

      r.push_back (move (ap));
    }

    return r;
  }

  // Merge dependency languages for the (ultimate) dependent of the specified
  // type.
  //
  static void
  merge_languages (const string& type,
                   small_vector<language, 1>& langs,
                   const available_package& ap)
  {
    for (const language& l: ap.effective_languages ())
    {
      // Unless both the dependent and dependency types are libraries, the
      // interface/implementation distinction does not apply.
      //
      bool lib (type == "lib" && ap.effective_type () == "lib");

      auto i (find_if (langs.begin (), langs.end (),
                       [&l] (const language& x)
                       {
                         return x.name == l.name;
                       }));

      if (i == langs.end ())
      {
        // If this is an implementation language for a dependency, then it is
        // also an implementation language for a dependent. The converse,
        // howevere, depends on whether this dependency is an interface or
        // imlementation of this dependent, which we do not know. So we have
        // to assume it's interface.
        //
        langs.push_back (language {l.name, lib && l.impl});
      }
      else
      {
        i->impl = i->impl && (lib && l.impl); // Merge.
      }
    }
  }

  // Collect dependencies of the specified package, potentially recursively.
  // System dependencies go to deps, non-system -- to pkgs, which could be the
  // same as deps or NULL, depending on the desired semantics (see the call
  // site for details). Find available packages for pkgs and deps and merge
  // languages.
  //
  static void
  collect_dependencies (const common_options& co,
                        database& db,
                        packages* pkgs,
                        packages& deps,
                        const string& type,
                        small_vector<language, 1>& langs,
                        const selected_package& p,
                        bool recursive)
  {
    for (const auto& pr: p.prerequisites)
    {
      const lazy_shared_ptr<selected_package>& ld (pr.first);

      // We only consider dependencies from target configurations, similar
      // to pkg-install.
      //
      database& pdb (ld.database ());
      if (pdb.type == host_config_type || pdb.type == build2_config_type)
        continue;

      shared_ptr<selected_package> d (ld.load ());

      // Packaging stuff that is spread over multiple configurations is just
      // too hairy so we don't support it. Specifically, it becomes tricky to
      // override build options since using a global override will also affect
      // host/build2 configurations.
      //
      if (db != pdb)
        fail << "dependency package " << *d << " belongs to different "
             << "configuration " << pdb.config_orig;

      // The selected package can only be configured if all its dependencies
      // are configured.
      //
      assert (d->state == package_state::configured);

      bool sys (d->substate == package_substate::system);
      packages* ps (sys ? &deps : pkgs);

      // Skip duplicates.
      //
      if (ps == nullptr || find_if (ps->begin (), ps->end (),
                                    [&d] (const package& p)
                                    {
                                      return p.selected == d;
                                    }) == ps->end ())
      {
        const selected_package& p (*d);

        if (ps != nullptr || (recursive && !sys))
        {
          available_packages aps (find_available_packages (co, db, d));

          // Load and merge languages.
          //
          if (recursive && !sys)
          {
            const shared_ptr<available_package>& ap (aps.front ().first);
            db.load (*ap, ap->languages_section);
            merge_languages (type, langs, *ap);
          }

          if (ps != nullptr)
          {
            dir_path out;
            if (ps != &deps)
              out = p.effective_out_root (db.config);

            ps->push_back (package {move (d), move (aps), move (out)});
          }
        }

        if (recursive && !sys)
          collect_dependencies (co, db, pkgs, deps, type, langs, p, recursive);
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
    enum class recursive_mode {auto_, full, separate};

    optional<recursive_mode> rec;
    {
      diag_record dr;

      if (o.recursive_specified ())
      {
        const string& m (o.recursive ());

        if      (m == "auto")     rec = recursive_mode::auto_;
        else if (m == "full")     rec = recursive_mode::full;
        else if (m == "separate") rec = recursive_mode::separate;
        else
          dr << fail << "unknown mode '" << m << "' specified with --recursive";
      }

      if (o.private_ ())
      {
        if (!rec)
        {
          dr << fail << "--private specified without --recursive";
        }
        else if (*rec == recursive_mode::separate)
        {
          dr << fail << "--private specified without --recursive=separate";
        }
      }

      if (!dr.empty ())
        dr << info << "run 'bpkg help pkg-bindist' for more information";
    }

    // Sort arguments into package names and configuration variables.
    //
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

      if (pns.empty ())
        fail << "package name argument expected" <<
          info << "run 'bpkg help pkg-bindist' for more information";
    }

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
           << "manager with --distribution" <<
        info << "specify --distribution=archive to generate installation "
           << "archive" <<
        info << "consider specifying --os-release-* if unable to correctly "
           << "auto-detect host operating system";
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

    // Generate one binary package.
    //
    struct result
    {
      paths    bins;
      packages deps;
      shared_ptr<selected_package> pkg;
    };

    auto generate = [&o, &vars,
                     rec, &spm,
                     &c, &db] (const vector<package_name>& pns,
                               bool first) -> result
    {
      // Resolve package names to selected packages and verify they are all
      // configured. While at it collect their available packages and
      // dependencies as well as figure out type and languages.
      //
      packages pkgs, deps;
      string type;
      small_vector<language, 1> langs;

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

        // Make sure there are no dependent configuration variables. The
        // rationale here is that we most likely don't want to generate a
        // binary package in a configuration that is specific to some
        // dependents.
        //
        if (!o.allow_dependent_config ())
        {
          for (const config_variable& v: p->config_variables)
          {
            switch (v.source)
            {
            case config_source::dependent:
              {
                fail << "configuration variable " << v.name << " is imposed "
                     << " by dependent package" <<
                  info << "specify it as user configuration to allow" <<
                  info << "or specify --allow-dependent-config" << endf;
              }
            case config_source::user:
            case config_source::reflect:
              break;
            }
          }
        }

        // Load the available package for type/languages as well as the
        // mapping information.
        //
        available_packages aps (find_available_packages (o, db, p));
        const shared_ptr<available_package>& ap (aps.front ().first);
        db.load (*ap, ap->languages_section);

        if (pkgs.empty ()) // First.
        {
          type = ap->effective_type ();
          langs = ap->effective_languages ();
        }
        else
          merge_languages (type, langs, *ap);

        const selected_package& r (*p);
        pkgs.push_back (
          package {move (p), move (aps), r.effective_out_root (db.config)});

        // If --recursive is not specified or specified with the seperate mode
        // then we want all the immediate (system and non-) dependecies in
        // deps. Otherwise, if the recursive mode is full, then we want all
        // the transitive non-system dependecies in pkgs. In both recursive
        // modes we also want all the transitive system dependecies in deps.
        //
        // Note also that in the auto recursive mode it's possible that some
        // of the system dependencies are not really needed. But there is no
        // way for us to detect this and it's better to over- than
        // under-specify.
        //
        collect_dependencies (
          o,
          db,
          (!rec || *rec == recursive_mode::separate
           ? &deps
           : *rec == recursive_mode::full ? &pkgs : nullptr),
          deps,
          type,
          langs,
          r,
          rec.has_value ());
      }

      // Load the package manifest (source of extra metadata). This should be
      // always possible since the package is configured and is not system.
      //
      const shared_ptr<selected_package>& sp (pkgs.front ().selected);

      package_manifest pm (
        pkg_verify (o,
                    sp->effective_src_root (db.config_orig),
                    true  /* ignore_unknown */,
                    false /* ignore_toolchain */,
                    false /* load_buildfiles */,
                    // Copy potentially fixed up version from selected package.
                    [&sp] (version& v) {v = sp->version;}));

      optional<bool> recursive_full;
      if (rec && *rec != recursive_mode::separate)
        recursive_full = *rec != recursive_mode::full;

      // Note that we pass type from here in case one day we want to provide
      // an option to specify/override it (along with languages). Note that
      // there will probably be no way to override type for dependencies.
      //
      paths r (spm->generate (pkgs,
                              deps,
                              vars,
                              db.config,
                              pm,
                              type, langs,
                              recursive_full,
                              first));

      return result {move (r), move (deps), move (pkgs.front ().selected)};
    };

    list<result> rs; // Note: list for reference stability.

    // Generate packages for dependencies, recursively, suppressing
    // duplicates. Note: recursive lambda.
    //
    auto generate_deps = [&generate, &rs] (const packages& deps,
                                           const auto& generate_deps) -> void
    {
      for (const package& d: deps)
      {
        const shared_ptr<selected_package>& p (d.selected);

        // Skip system dependencies.
        //
        if (p->substate == package_substate::system)
          continue;

        // Make sure we don't generate the same dependency multiple times.
        //
        if (find_if (rs.begin (), rs.end (),
                     [&p] (const result& r)
                     {
                       return r.pkg == p;
                     }) != rs.end ())
          continue;

        if (verb >= 1)
          text << "generating package for dependency " << p->name;

        rs.push_back (generate ({p->name}, false /* first */));
        generate_deps (rs.back ().deps, generate_deps);
      }
    };

    // Generate top-level package(s).
    //
    rs.push_back (generate (pns, true /* first */));

    // Generate dependencies, if requested.
    //
    if (rec && rec == recursive_mode::separate)
      generate_deps (rs.back ().deps, generate_deps);

    t.commit ();

    if (rs.front ().bins.empty ())
      return 0; // Assume prepare-only mode or similar.

    if (verb && !o.no_result ())
    {
      const string& d (o.distribution_specified ()
                       ? o.distribution ()
                       : spm->os_release.name_id);

      for (auto b (rs.begin ()), i (b); i != rs.end (); ++i)
      {
        const selected_package& p (*i->pkg);

        diag_record dr (text);

        dr << "generated " << d << " package for "
           << (i != b ? "dependency " : "")
           << p.name << '/' << p.version << ':';

        for (const path& p: i->bins)
          dr << "\n  " << p;
      }
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
