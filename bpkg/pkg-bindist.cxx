// file      : bpkg/pkg-bindist.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-bindist.hxx>

#include <map>
#include <list>
#include <iostream> // cout

#include <libbutl/json/serializer.hxx>

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

  enum class recursive_mode {auto_, full, separate};

  // Package-specific recursive mode overrides.
  //
  struct package_recursive
  {
    // --recursive <pkg>=<mode>
    //
    // If present, overrides the recursive mode for collecting dependencies of
    // this package (inner absent means `none`).
    //
    optional<optional<recursive_mode>> dependencies;

    // --recursive ?<pkg>=<mode>
    //
    // If present, this dependency is collected in this mode rather than in
    // the mode(s) its dependents collect their dependencies (inner absent
    // means `none`).
    //
    optional<optional<recursive_mode>> self;
  };

  using package_recursive_map = map<package_name, package_recursive>;

  // Collect dependencies of the specified package, potentially recursively.
  //
  // Specifically, in the non-recursive mode or in the `separate` recursive
  // mode we want all the immediate (system and non-) dependencies in deps.
  // Otherwise, if the recursive mode is `full`, then we want all the
  // transitive non-system dependencies in pkgs. In both recursive modes we
  // also want all the transitive system dependencies in deps.
  //
  // Or, to put it another way, the system dependencies and those collected
  // non-recursively or in the `separate` recursive mode go to the deps
  // list. The dependencies collected in the `full` recursive mode go to pkgs
  // list. All other dependencies (collected in the `auto` recursive mode) are
  // not saved to any of the lists.
  //
  // Find available packages for pkgs and deps and merge languages. Also save
  // the effective recursive modes to package_rec_map (so that the mode from
  // the first encounter of the package is used in subsequent).
  //
  static void
  collect_dependencies (const common_options& co,
                        database& db,
                        packages& pkgs,
                        packages& deps,
                        const string& type,
                        small_vector<language, 1>& langs,
                        const selected_package& p,
                        optional<recursive_mode> rec,
                        package_recursive_map& package_rec_map)
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

      // Deduce/save the effective recursive modes for the dependency.
      //
      // Note: don't change after being saved from the command line
      // (--recursive [?]<pkg>=<mode>) or via the first encountered dependent.
      //
      optional<recursive_mode> drec;
      optional<recursive_mode> srec;

      if (!sys)
      {
        package_recursive& pr (package_rec_map[d->name]);

        if (!pr.dependencies)
          pr.dependencies = rec;

        if (!pr.self)
          pr.self = rec;

        drec = *pr.dependencies;
        srec = *pr.self;
      }

      // Note that in the `auto` recursive mode it's possible that some of the
      // system dependencies are not really needed. But there is no way for us
      // to detect this and it's better to over- than under-specify.
      //
      packages* ps (!srec || *srec == recursive_mode::separate
                    ? &deps
                    : *srec == recursive_mode::full ? &pkgs : nullptr);

      // Collect the package dependencies recursively, if requested, unless
      // the package is collected in the separate mode in which case its
      // dependencies will be collected later, when its own binary package is
      // generated.
      //
      bool recursive (drec.has_value () &&
                      (!srec || *srec != recursive_mode::separate));

      // Skip duplicates.
      //
      if (ps == nullptr || find_if (ps->begin (), ps->end (),
                                    [&d] (const package& p)
                                    {
                                      return p.selected == d;
                                    }) == ps->end ())
      {
        const selected_package& p (*d);

        if (ps != nullptr || recursive)
        {
          available_packages aps (find_available_packages (co, db, d));

          // Load and merge languages.
          //
          if (recursive)
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

        if (recursive)
          collect_dependencies (co,
                                db,
                                pkgs, deps, type, langs, p,
                                drec, package_rec_map);
      }
    }
  }

  int
  pkg_bindist (const pkg_bindist_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_bindist");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    // Parse and verify options.
    //
    map<package_name, package_recursive> package_rec_map;

    optional<recursive_mode> rec;
    {
      diag_record dr;

      for (const string& m: o.recursive ())
      {
        if      (m == "auto")     rec = recursive_mode::auto_;
        else if (m == "full")     rec = recursive_mode::full;
        else if (m == "separate") rec = recursive_mode::separate;
        else if (m == "none")     rec = nullopt;
        else
        {
          size_t n (m.find ('='));

          if (n != string::npos)
          {
            string pm (m, n + 1, m.size () - n - 1);

            optional<optional<recursive_mode>> prec;

            if      (pm == "auto")     prec = recursive_mode::auto_;
            else if (pm == "full")     prec = recursive_mode::full;
            else if (pm == "separate") prec = recursive_mode::separate;
            else if (pm == "none")     prec = optional<recursive_mode> ();

            if (prec)
            {
              string p (m, 0, n);
              bool dependency (p[0] == '?');

              if (dependency)
                p.erase (0, 1);

              try
              {
                package_recursive& pr (
                  package_rec_map[project_name (move (p))]);

                (dependency ? pr.self : pr.dependencies) = prec;
              }
              catch (const invalid_argument& e)
              {
                dr << fail << "invalid package name '" << p
                   << "' in --recursive mode '" << m << "': " << e;
                break;
              }

              continue; // Proceed to the next --recursive option.
            }

            // Fall through.
          }

          dr << fail << "unknown --recursive mode '" << m << "'";
          break;
        }
      }

      // Verify the --private/--recursive options consistency for the simple
      // case (no --recursive [?]<pkg>=<mode>). Otherwise, just ignore
      // --private if the dependencies are not bundled.
      //
      if (o.private_ () && package_rec_map.empty ())
      {
        if (!rec)
        {
          dr << fail << "--private specified without --recursive";
        }
        else if (*rec == recursive_mode::separate)
        {
          dr << fail << "--private specified with --recursive=separate";
        }
      }

      if (!dr.empty ())
        dr << info << "run 'bpkg help pkg-bindist' for more information";
    }

    if (o.structured_result_specified ())
    {
      if (o.no_result ())
        fail << "both --structured-result and --no-result specified";

      if (o.structured_result () != "json")
        fail << "unknown --structured-result format '"
             << o.structured_result () << "'";
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
    pair<unique_ptr<system_package_manager>, string> spm (
      make_production_system_package_manager (o,
                                              host_triplet,
                                              o.distribution (),
                                              o.architecture ()));
    if (spm.first == nullptr)
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
    using binary_file = system_package_manager::binary_file;
    using binary_files = system_package_manager::binary_files;

    struct result
    {
      binary_files bins;
      packages     deps;
      shared_ptr<selected_package> pkg;
    };

    bool dependent_config (false);

    auto generate = [&o, &vars,
                     &package_rec_map, &spm,
                     &c, &db,
                     &dependent_config] (const vector<package_name>& pns,
                                         optional<recursive_mode> rec,
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
        for (const config_variable& v: p->config_variables)
        {
          switch (v.source)
          {
          case config_source::dependent:
            {
              if (!o.allow_dependent_config ())
              {
                fail << "configuration variable " << v.name << " is imposed "
                     << " by dependent package" <<
                  info << "specify it as user configuration to allow" <<
                  info << "or specify --allow-dependent-config";
              }

              dependent_config = true;
              break;
            }
          case config_source::user:
          case config_source::reflect:
            break;
          }

          if (dependent_config)
            break;
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

        // Deduce the effective recursive mode for collecting dependencies.
        //
        optional<recursive_mode> drec;
        {
          auto i (package_rec_map.find (n));

          if (i != package_rec_map.end ())
          {
            const package_recursive& pr (i->second);
            drec = pr.dependencies ? *pr.dependencies : rec;
          }
          else
            drec = rec;
        }

        collect_dependencies (
          o,
          db,
          pkgs,
          deps,
          type,
          langs,
          r,
          drec,
          package_rec_map);
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
        recursive_full = (*rec == recursive_mode::full);

      // Only enable private installation subdirectory functionality if the
      // dependencies are bundled with the dependent (see the --private option
      // for details).
      //
      bool priv (o.private_ () &&
                 rec           &&
                 (*rec == recursive_mode::full ||
                  *rec == recursive_mode::auto_));

      // Note that we pass type from here in case one day we want to provide
      // an option to specify/override it (along with languages). Note that
      // there will probably be no way to override type for dependencies.
      //
      binary_files r (spm.first->generate (pkgs,
                                           deps,
                                           vars,
                                           db.config,
                                           pm,
                                           type, langs,
                                           recursive_full,
                                           priv,
                                           first));

      return result {move (r), move (deps), move (pkgs.front ().selected)};
    };

    list<result> rs; // Note: list for reference stability.

    // Generate packages for dependencies, recursively, suppressing
    // duplicates. Note: recursive lambda.
    //
    auto generate_deps = [&package_rec_map, &generate, &rs]
                         (const packages& deps,
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

        // The effective recursive modes for the dependency.
        //
        optional<recursive_mode> drec;
        optional<recursive_mode> srec;
        {
          auto i (package_rec_map.find (p->name));

          // Must have been saved by collect_dependencies().
          //
          assert (i != package_rec_map.end () &&
                  i->second.self              &&
                  i->second.dependencies);

          drec = *i->second.dependencies;
          srec = *i->second.self;
        }

        // See collect_dependencies() for details.
        //
        assert (!srec || *srec == recursive_mode::separate);

        if (srec)
        {
          rs.push_back (generate ({p->name}, drec, false /* first */));
          generate_deps (rs.back ().deps, generate_deps);
        }
      }
    };

    // Generate top-level package(s).
    //
    rs.push_back (generate (pns, rec, true /* first */));

    // Generate dependencies, if requested.
    //
    generate_deps (rs.back ().deps, generate_deps);

    t.commit ();

    if (rs.front ().bins.empty ())
      return 0; // Assume prepare-only mode or similar.

    if (o.no_result ())
      ;
    else if (!o.structured_result_specified ())
    {
      if (verb)
      {
        const string& d (o.distribution_specified ()
                         ? o.distribution ()
                         : spm.first->os_release.name_id);

        for (auto b (rs.begin ()), i (b); i != rs.end (); ++i)
        {
          const selected_package& p (*i->pkg);

          string ver (p.version.string (false /* ignore_revision */,
                                        true  /* ignore_iteration */));

          diag_record dr (text);

          dr << "generated " << d << " package for "
             << (i != b ? "dependency " : "")
             << p.name << '/' << ver << ':';

          for (const binary_file& f: i->bins)
            dr << "\n  " << f.path;
        }
      }
    }
    else
    {
      json::stream_serializer s (cout);

      auto member = [&s] (const char* n, const string& v, const char* d = "")
      {
        if (v != d)
          s.member (n, v);
      };

      auto package = [&s, &member] (const result& r)
      {
        const selected_package& p (*r.pkg);
        const binary_files& bfs (r.bins);

        string ver (p.version.string (false /* ignore_revision */,
                                      true  /* ignore_iteration */));

        s.begin_object (); // package
        {
          member ("name",    p.name.string ());
          member ("version", ver);
          member ("system_version", bfs.system_version);
          s.member_begin_array ("files");
          for (const binary_file& bf: bfs)
          {
            s.begin_object (); // file
            {
              member ("type", bf.type);
              member ("path", bf.path.string ());
              member ("system_name", bf.system_name);
            }
            s.end_object (); // file
          };
          s.end_array ();
        }
        s.end_object (); // package
      };

      s.begin_object (); // bindist_result
      {
        member ("distribution", spm.second);
        member ("architecture", spm.first->arch);

        s.member_begin_object ("os_release");
        {
          const auto& r (spm.first->os_release);

          member ("name_id", r.name_id);

          if (!r.like_ids.empty ())
          {
            s.member_begin_array ("like_ids");
            for (const string& id: r.like_ids) s.value (id);
            s.end_array ();
          }

          member ("version_id", r.version_id);
          member ("variant_id", r.variant_id);

          member ("name",             r.name);
          member ("version_codename", r.version_codename);
          member ("variant",          r.variant);
        }
        s.end_object (); // os_release

        if (rec)
          member ("recursive", *rec == recursive_mode::auto_ ? "auto" :
                               *rec == recursive_mode::full  ? "full" :
                               "separate");

        if (o.private_ ()) s.member ("private", true);
        if (dependent_config) s.member ("dependent_config", true);

        s.member_name ("package");
        package (rs.front ());

        if (rs.size () > 1)
        {
          s.member_begin_array ("dependencies");
          for (auto i (rs.begin ()); ++i != rs.end (); ) package (*i);
          s.end_array ();
        }
      }
      s.end_object (); // bindist_result

      cout << endl;
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
