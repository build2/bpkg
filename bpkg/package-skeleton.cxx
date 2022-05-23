// file      : bpkg/package-skeleton.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package-skeleton.hxx>

#include <sstream>

#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/file.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/operation.hxx>

#include <libbuild2/lexer.hxx>
#include <libbuild2/parser.hxx>

#include <libbuild2/config/utility.hxx>

#include <bpkg/package.hxx>
#include <bpkg/database.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  // These are defined in bpkg.cxx and initialized in main().
  //
  extern strings                build2_cmd_vars;
  extern build2::scheduler      build2_sched;
  extern build2::global_mutexes build2_mutexes;
  extern build2::file_cache     build2_fcache;

  void
  build2_init (const common_options&);

  package_skeleton::
  ~package_skeleton ()
  {
  }

  package_skeleton::
  package_skeleton (package_skeleton&& v)
  {
    *this = move (v);
  }

  package_skeleton& package_skeleton::
  operator= (package_skeleton&& v)
  {
    if (this != &v)
    {
      co_ = v.co_;
      db_ = v.db_;
      available_ = v.available_;
      config_vars_ = move (v.config_vars_);
      config_srcs_ = v.config_srcs_;
      src_root_ = move (v.src_root_);
      out_root_ = move (v.out_root_);
      created_ = v.created_;
      ctx_ = move (v.ctx_);
      rs_ = v.rs_;
      cmd_vars_ = move (v.cmd_vars_);
      reflect_names_ = move (v.reflect_names_);
      reflect_vars_ = move (v.reflect_vars_);
      reflect_frag_ = move (v.reflect_frag_);

      v.db_ = nullptr;
      v.available_ = nullptr;
    }

    return *this;
  }

  package_skeleton::
  package_skeleton (const package_skeleton& v)
      : co_ (v.co_),
        db_ (v.db_),
        available_ (v.available_),
        config_vars_ (v.config_vars_),
        config_srcs_ (v.config_srcs_),
        src_root_ (v.src_root_),
        out_root_ (v.out_root_),
        created_ (v.created_),
        cmd_vars_ (v.cmd_vars_),
        reflect_names_ (v.reflect_names_),
        reflect_vars_ (v.reflect_vars_),
        reflect_frag_ (v.reflect_frag_)
  {
    // The idea here is to create an "unloaded" copy but with enough state
    // that it can be loaded if necessary.
  }

  package_skeleton::
  package_skeleton (const common_options& co,
                    database& db,
                    const available_package& ap,
                    strings cvs,
                    const vector<config_variable>* css,
                    optional<dir_path> src_root,
                    optional<dir_path> out_root)
      : co_ (&co),
        db_ (&db),
        available_ (&ap),
        config_vars_ (move (cvs)),
        config_srcs_ (css)
  {
    // Should not be created for stubs.
    //
    assert (available_->bootstrap_build);

    // We are only interested in old user configuration variables.
    //
    if (config_srcs_ != nullptr)
    {
      if (find_if (config_srcs_->begin (), config_srcs_->end (),
                   [] (const config_variable& v)
                   {
                     return v.source == config_source::user;
                   }) == config_srcs_->end ())
        config_srcs_ = nullptr;
    }

    if (src_root)
    {
      src_root_ = move (*src_root);

      if (out_root)
        out_root_ = move (*out_root);
    }
    else
      assert (!out_root);
  }

  // Print the location of a depends value in the specified manifest file.
  //
  // Note that currently we only use this function for the external packages.
  // We could also do something similar for normal packages by pointing to the
  // manifest we have serialized. In this case we would also need to make sure
  // the temp directory is not cleaned in case of an error. Maybe one day.
  //
  static void
  depends_location (const diag_record& dr,
                    const path& mf,
                    size_t depends_index)
  {
    // Note that we can't do much on the manifest parsing failure and just
    // skip printing the location in this case.
    //
    try
    {
      ifdstream is (mf);
      manifest_parser p (is, mf.string ());

      manifest_name_value nv (p.next ());
      if (nv.name.empty () && nv.value == "1")
      {
        size_t i (0);
        for (nv = p.next (); !nv.empty (); nv = p.next ())
        {
          if (nv.name == "depends" && i++ == depends_index)
          {
            dr << info (location (p.name (),
                                  nv.value_line,
                                  nv.value_column))
               << "depends value defined here";
            break;
          }
        }
      }
    }
    catch (const manifest_parsing&) {}
    catch (const io_error&) {}
  }

  bool package_skeleton::
  evaluate_enable (const string& cond, size_t depends_index)
  {
    try
    {
      using namespace build2;
      using build2::fail;
      using build2::endf;

      scope& rs (load ());

      istringstream is ('(' + cond + ')');
      is.exceptions (istringstream::failbit | istringstream::badbit);

      // Location is tricky: theoretically we can point to the exact position
      // of an error but that would require quite hairy and expensive manifest
      // re-parsing. The really bad part is that all this effort will be
      // wasted in the common "no errors" cases. So instead we do this
      // re-parsing lazily from the diag frame.
      //
      path_name in ("<depends-enable-clause>");
      uint64_t il (1);

      auto df = build2::make_diag_frame (
        [&cond, &rs, depends_index] (const diag_record& dr)
        {
          dr << info << "enable condition: (" << cond << ")";

          // For external packages we have the manifest so print the location
          // of the depends value in questions.
          //
          if (!rs.out_eq_src ())
            depends_location (dr,
                              rs.src_path () / manifest_file,
                              depends_index);
        });

      lexer l (is, in, il /* start line */);
      parser p (rs.ctx);
      value v (p.parse_eval (l, rs, rs, parser::pattern_mode::expand));

      try
      {
        // Should evaluate to 'true' or 'false'.
        //
        return build2::convert<bool> (move (v));
      }
      catch (const invalid_argument& e)
      {
        fail (build2::location (in, il)) << e << endf;
      }
    }
    catch (const build2::failed&)
    {
      throw failed (); // Assume the diagnostics has already been issued.
    }
  }

  // Serialize a variable assignment for a buildfile fragment.
  //
  static void
  serialize_buildfile (string& r,
                       const string& var, const build2::value& val,
                       build2::names& storage)
  {
    using namespace build2;

    r += var;
    r += " = ";

    if (val.null)
      r += "[null]";
    else
    {
      storage.clear ();
      names_view nv (reverse (val, storage));

      if (!nv.empty ())
      {
        ostringstream os;
        to_stream (os, nv, quote_mode::normal, '@');
        r += os.str ();
      }
    }

    r += '\n';
  }

  // Serialize a variable assignment for a command line override.
  //
  static string
  serialize_cmdline (const string& var, const build2::value& val,
                     build2::names& storage)
  {
    using namespace build2;

    string r (var + '=');

    if (val.null)
      r += "[null]";
    else
    {
      storage.clear ();
      names_view nv (reverse (val, storage));

      if (!nv.empty ())
      {
        // Note: we need to use command-line (effective) quoting.
        //
        ostringstream os;
        to_stream (os, nv, quote_mode::effective, '@');
        r += os.str ();
      }
    }

    return r;
  }

  void package_skeleton::
  evaluate_reflect (const string& refl, size_t depends_index)
  {
    // The reflect configuration variables are essentially overrides that will
    // be passed on the command line when we configure the package. They could
    // clash with configuration variables specified by the user (config_vars_)
    // and it feels like user values should take precedence. Though one could
    // also argue we should diagnose this case and fail not to cause more
    // confusion.
    //
    // It seems like the most straightforward way to achieve the desired
    // semantics with the mechanisms that we have (in other words, without
    // inventing another "level" of overrides) is to evaluate the reflect
    // fragment after loading root.build. This way it will (1) be able to use
    // variables set by root.build in conditions, (2) override default values
    // of configuration variables (and those loaded from config.build), and
    // (3) be overriden by configuration variables specified by the user.
    // Naturally, this approach is not without a few corner cases:
    //
    // 1. Append in the reflect clause may not produce the desired result
    //    (i.e., it will append to the default value in root.build) rather
    //    than overriding it, as would have happen if it were a real variable
    //    override.
    //
    //    config.hello.x ?= 1 # root.build
    //    config.hello.x += 2 # reflect clause
    //
    // We may also have configuration values from the previous reflect clause
    // which we want to "factor in" before evaluating the next enable or
    // reflect clauses (so that they can use the previously reflect values or
    // values that are derived from them in root.build). It seems like we have
    // two options here: either enter them as true overrides similar to
    // config_vars_ or just evaluate them similar to loading config.build
    // (which, BTW, we might have, in case of an external package). The big
    // problem with the former approach is that it will then prevent any
    // further reflect clauses from modifying the same values.
    //
    // So overall it feels like we have iterative/compartmentalized
    // configuration process. A feedback loop, in a sense. And it's the
    // responsibility of the package author (who is in control of both
    // root.build and manifest) to arrange for suitable compartmentalization.
    //
    // BTW, a plan B would be to restrict reflect to just config vars in which
    // case we could merge them with true overrides. Though how exactly would
    // we do this merging is unclear.
    //
    try
    {
      // Note: similar in many respects to evaluate_enable().
      //
      using namespace build2;
      using build2::fail;
      using build2::endf;

      // Merge old configuration variables into config_vars since otherwise
      // they may end up being overridden by reflects.
      //
      if (config_srcs_ != nullptr)
        merge_old_config_vars ();

      scope& rs (load ());

      istringstream is (refl);
      is.exceptions (istringstream::failbit | istringstream::badbit);

      path_name in ("<depends-reflect-clause>");
      uint64_t il (1);

      auto df = build2::make_diag_frame (
        [&refl, &rs, depends_index] (const diag_record& dr)
        {
          // Probably safe to assume a one-line fragment contains a variable
          // assignment.
          //
          if (refl.find ('\n') == string::npos)
            dr << info << "reflect variable: " << refl;
          else
            dr << info << "reflect fragment:\n"
               << refl;

          // For external packages we have the manifest so print the location
          // of the depends value in questions.
          //
          if (!rs.out_eq_src ())
            depends_location (dr,
                              rs.src_path () / manifest_file,
                              depends_index);
        });

      // Note: a lot of this code is inspired by the config module.
      //

      // Collect all the config.<name>.* variables on the first pass and
      // filter out unchanged on the second.
      //
      auto& vp (rs.var_pool ());
      const variable& ns (vp.insert ("config." + name ().variable ()));

      struct value_data
      {
        const value* val;
        size_t       ver;
      };

      map<const variable*, value_data> vars;

      auto process = [&rs, &ns, &vars] (bool collect)
      {
        for (auto p (rs.vars.lookup_namespace (ns));
             p.first != p.second;
             ++p.first)
        {
          const variable& var (p.first->first);

          // This can be one of the overrides (__override, __prefix, etc),
          // which we skip.
          //
          if (var.override ())
            continue;

          // What happens to version if overriden? A: appears to be still
          // incremented!
          //
          const variable_map::value_data& val (p.first->second);

          if (collect)
          {
            vars.emplace (&var, value_data {nullptr, val.version});
          }
          else
          {
            auto i (vars.find (&var));

            if (i != vars.end ())
            {
              if (i->second.ver == val.version)
                vars.erase (i); // Unchanged.
              else
                i->second.val = &val;
            }
            else
              vars.emplace (&var, value_data {&val, 0});
          }
        }
      };

      process (true);

      lexer l (is, in, il /* start line */);
      parser p (rs.ctx);
      p.parse_buildfile (l, &rs, rs);

      process (false);

      // Add to the map the reflect variables collected previously.
      //
      for (string& n: reflect_names_)
      {
        auto p (vars.emplace (&vp.insert (move (n)), value_data {nullptr, 0}));

        if (p.second)
        {
          // The value got to be there since it's set by the accumulated
          // fragment we've evaluated before root.build.
          //
          p.first->second.val = rs.vars[p.first->first].value;
        }
      }

      // Re-populate everything from the map.
      //
      reflect_names_.clear ();
      reflect_frag_.clear ();

#if 0
      // NOTE: see also collect_config() if enabling this.
      //
      reflect_vars_.clear ();
#else
      reflect_vars_ = config_vars_;
#endif

      // Collect the config.<name>.* variables that were changed by this
      // and previous reflect clauses.
      //
      // Specifically, we update both the accumulated fragment to be evaluated
      // before root.build on the next load (if any) as well as the merged
      // configuration variable overrides to be passed during the package
      // configuration. Doing both of these now (even though one of them won't
      // be needed) allows us to immediately drop the build context and
      // release its memory. It also makes the implementation a bit simpler
      // (see, for example, the copy constructor).
      //
      names storage;
      for (const auto& p: vars)
      {
        const variable& var (*p.first);
        const value& val (*p.second.val);

        reflect_names_.push_back (var.name);

        // For the accumulated fragment we always save the original and let
        // the standard overriding take its course.
        //
        serialize_buildfile (reflect_frag_, var.name, val, storage);

        // For the accumulated overrides we have to merge user config_vars_
        // with reflect values. Essentially, we have three possibilities:
        //
        // 1. There is no corresponding reflect value for a user value. In
        //    this case we just copy over the user value.
        //
        // 2. There is no corresponding user value for a reflect value. In
        //    this case we just copy over the reflect value.
        //
        // 3. There are both reflect and user values. In this case we replace
        //    the user value with the final (overriden) value using plain
        //    assignment (`=`). We do it this way to cover append overrides,
        //    for example:
        //
        //    config.hello.backend = foo  # reflect
        //    config.hello.backend += bar # user
        //
        pair<lookup, size_t> org {lookup {val, var, rs.vars}, 1 /* depth */};
        pair<lookup, size_t> ovr;

        if (var.overrides == nullptr)
          ovr = org; // Case #2.
        else
        {
          // NOTE: see also above and below if enabling this.
          //
#if 0
          // Case #3.
          //
          // The override can come from two places: config_vars_ or one of the
          // "global" sources (environment variable, default options file; see
          // load() for details). The latter can only be a global override and
          // can be found (together with global overrides from config_vars_)
          // in context::global_var_overrides.
          //
          // It feels like mixing global overrides and reflect is misguided:
          // we probably don't want to rewrite it with a global override (per
          // case #3 above) since it will apply globally. So let's diagnose it
          // for now.
          //
          {
            const strings& ovs (ctx_->global_var_overrides);
            auto i (find_if (ovs.begin (), ovs.end (),
                             [&var] (const string& o)
                             {
                               // TODO: extracting name is not easy.
                             }));

            if (i != ovs.end ())
            {
              fail << "global override for reflect clause variable " << var <<
                info << "global override: " << *i;
            }
          }

          // Ok, this override must be present in config_vars_.
          //
          // @@ Extracting the name from config_vars_ and similar is not easy:
          //    they are buildfile fragments and context actually parses them.
          //
          // @@ What if we have multiple overrides?
          //
          // @@ What if it's some scoped override or some such (e.g., all
          //    these .../x=y, etc).
          //
          // @@ Does anything change if we have an override but it does not
          //    apply (i.e., ovr == org && var.overrides != nullptr)?
          //
          // @@ Perhaps a sensible approach is to start relaxing/allowing
          //    this for specific, sensible cases (e.g., single unqualified
          //    override)?
          //
          // What would be the plausible scenarios for an override?
          //
          // 1. Append override that adds some backend or some such to the
          //    reflect value.
          //
          // 2. A reflect may enable a feature based on the dependency
          //    alternative selected (e.g., I see we are using Qt6 so we might
          //    as well enable feature X). The user may want do disable it
          //    with an override.
          //
          ovr = rs.lookup_override (var, org);
#else
          fail << "command line override of reflect clause variable " << var
               << endf;
#endif
        }

        reflect_vars_.push_back (
          serialize_cmdline (var.name, *ovr.first, storage));
      }

#if 0
      // TODO: copy over config_vars_ that are not in the map (case #1).
#endif

      // Drop the build system state since it needs reloading (some computed
      // values in root.build may depend on the new configuration values).
      //
      ctx_ = nullptr;
    }
    catch (const build2::failed&)
    {
      throw failed (); // Assume the diagnostics has already been issued.
    }
  }

  pair<strings, vector<config_variable>> package_skeleton::
  collect_config () &&
  {
    assert (db_ != nullptr); // Must be called only once.

    strings vars;
    vector<config_variable> srcs;

    // Check whether the user-specified configuration variable is a project
    // variables (i.e., its name start with config.<project>).
    //
    // Note that some user-specified variables may have qualifications
    // (global, scope, etc) but there is no reason to expect any project
    // configuration variables to use such qualifications (since they can only
    // apply to one project). So we ignore all qualified variables.
    //
    auto prj_var = [this, p = optional<string> ()] (const string& v) mutable
    {
      if (!p)
        p = "config." + name ().variable ();

      size_t n (p->size ());

      return v.compare (0, n, *p) == 0 && strchr (".=+ \t", v[n]) != nullptr;
    };

    if (!reflect_vars_.empty ())
    {
      assert (config_srcs_ == nullptr); // Should have been merged.

      vars = move (reflect_vars_);

      // Note that if the reflect variables list is not empty, then it also
      // contains the user-specified configuration variables, which all come
      // first (see above).
      //
      size_t nc (config_vars_.size ());

      if (!vars.empty ())
      {
        srcs.reserve (vars.size ()); // At most that many.

        // Assign the user source only to user-specified configuration
        // variables which are project variables (i.e., names start with
        // config.<project>). Assign the reflect source to all variables that
        // follow the user-specified configuration variables (which can only
        // be project variables).
        //
        for (size_t j (0); j != vars.size (); ++j)
        {
          const string& v (vars[j]);

          config_source s;

          if (j < nc)
          {
            if (prj_var (v))
              s = config_source::user;
            else
              continue;
          }
          else
            s = config_source::reflect;

          size_t p (v.find_first_of ("=+ \t"));
          assert (p != string::npos);

          string n (v, 0, p);

          // Check for a duplicate.
          //
          auto i (find_if (srcs.begin (), srcs.end (),
                           [&n] (const config_variable& cv)
                           {
                             return cv.name == n;
                           }));

          if (i != srcs.end ())
            assert (i->source == s); // See evaluate_reflect() for details.
          else
            srcs.push_back (config_variable {move (n), s});
        }
      }
    }
    else
    {
      vars = move (config_vars_);

      // If we don't have any reflect variables, then we don't really need to
      // load user variables from config.build (or equivalent) and add them to
      // config_vars since there is nothing for them to possibly override. So
      // all we need to do is to add user variables from config_vars.
      //
      if (!vars.empty () || config_srcs_ != nullptr)
      {
        srcs.reserve ((config_srcs_ != nullptr ? config_srcs_->size () : 0)
                      + vars.size ()); // At most that many.

        if (config_srcs_ != nullptr)
          for (const config_variable& v: *config_srcs_)
            if (v.source == config_source::user)
              srcs.push_back (v);

        for (const string& v: vars)
        {
          // Similar logic to the above case.
          //
          if (prj_var (v))
          {
            size_t p (v.find_first_of ("=+ \t"));
            assert (p != string::npos);

            string n (v, 0, p);

            // Check for a duplicate.
            //
            auto i (find_if (srcs.begin (), srcs.end (),
                             [&n] (const config_variable& cv)
                             {
                               return cv.name == n;
                             }));

            if (i == srcs.end ())
              srcs.push_back (config_variable {move (n), config_source::user});
          }
        }
      }
    }

    ctx_ = nullptr; // In case we only had conditions.
    db_ = nullptr;
    available_ = nullptr;

    return make_pair (move (vars), move (srcs));
  }

  // Note: cannot be package_skeleton member function due to iterator return
  // (build2 stuff is only forward-declared in the header).
  //
  static build2::scope_map::iterator
  bootstrap (package_skeleton& skl, const strings& cmd_vars)
  {
    assert (skl.ctx_ == nullptr);

    // The overall plan is as follows:
    //
    // 0. Create filesystem state if necessary (could have been created by
    //    another instance, e.g., during simulation).
    //
    // 1. Bootstrap the package skeleton.
    //
    // Creating a new context is not exactly cheap (~1.2ms debug, 0.08ms
    // release) so we could try to re-use it by cleaning all the scopes other
    // than the global scope (and probably some other places, like var pool).
    // But we will need to carefully audit everything to make sure we don't
    // miss anything (like absolute scope variable overrides being lost). So
    // maybe, one day, if this really turns out to be a performance issue.

    // Create the skeleton filesystem state, if it doesn't exist yet.
    //
    if (!skl.created_)
    {
      const available_package& ap (*skl.available_);

      // Note that we create the skeleton directories in the skeletons/
      // subdirectory of the configuration temporary directory to make sure
      // they never clash with other temporary subdirectories (git
      // repositories, etc).
      //
      if (skl.src_root_.empty () || skl.out_root_.empty ())
      {
        // Cannot be specified if src_root_ is unspecified.
        //
        assert (skl.out_root_.empty ());

        auto i (tmp_dirs.find (skl.db_->config_orig));
        assert (i != tmp_dirs.end ());

        // Make sure the source and out root directories, if set, are absolute
        // and normalized.
        //
        // Note: can never fail since the temporary directory should already
        // be created and so its path should be valid.
        //
        dir_path d (normalize (i->second, "temporary directory"));

        d /= "skeletons";
        d /= skl.name ().string () + '-' + ap.version.string ();

        if (skl.src_root_.empty ())
          skl.src_root_ = move (d); // out_root_ is the same.
        else
          skl.out_root_ = move (d); // Don't even need to create it.
      }

      if (!exists (skl.src_root_))
      {
        // Create the buildfiles.
        //
        // Note that it's probably doesn't matter which naming scheme to use
        // for the buildfiles, unless in the future we allow specifying
        // additional files.
        //
        {
          path bf (skl.src_root_ / std_bootstrap_file);

          mk_p (bf.directory ());

          // Save the {bootstrap,root}.build files.
          //
          auto save = [] (const string& s, const path& f)
          {
            try
            {
              ofdstream os (f);
              os << s;
              os.close ();
            }
            catch (const io_error& e)
            {
              fail << "unable to write to " << f << ": " << e;
            }
          };

          save (*ap.bootstrap_build, bf);

          if (ap.root_build)
            save (*ap.root_build, skl.src_root_ / std_root_file);
        }

        // Create the manifest file containing the bare minimum of values
        // which can potentially be required to load the build system state
        // (i.e., either via the version module or manual version extraction).
        //
        {
          package_manifest m;
          m.name = skl.name ();
          m.version = ap.version;

          // Note that there is no guarantee that the potential build2
          // constraint has already been verified. Thus, we also serialize the
          // build2 dependency value, letting the version module verify the
          // constraint.
          //
          // Also note that the resulting file is not quite a valid package
          // manifest, since it doesn't contain all the required values
          // (summary, etc). It, however, is good enough for build2 which
          // doesn't perform exhaustive manifest validation.
          //
          m.dependencies.reserve (ap.dependencies.size ());
          for (const dependency_alternatives_ex& das: ap.dependencies)
          {
            // Skip the the special (inverse) test dependencies.
            //
            if (!das.type)
              m.dependencies.push_back (das);
          }

          path mf (skl.src_root_ / manifest_file);

          try
          {
            ofdstream os (mf);
            manifest_serializer s (os, mf.string ());
            m.serialize (s);
            os.close ();
          }
          catch (const manifest_serialization& e)
          {
            // We shouldn't be creating a non-serializable manifest, since
            // it's crafted from the parsed values.
            //
            assert (false);

            fail << "unable to serialize " << mf << ": " << e.description;
          }
          catch (const io_error& e)
          {
            fail << "unable to write to " << mf << ": " << e;
          }
        }
      }

      skl.created_ = true;
    }

    // Initialize the build system.
    //
    if (!build2_sched.started ())
      build2_init (*skl.co_);

    try
    {
      using namespace build2;
      using build2::fail;
      using build2::endf;

      // Create build context.
      //
      skl.ctx_.reset (
        new context (build2_sched,
                     build2_mutexes,
                     build2_fcache,
                     false /* match_only */,          // Shouldn't matter.
                     false /* no_external_modules */,
                     false /* dry_run */,             // Shouldn't matter.
                     false /* keep_going */,          // Shouldnt' matter.
                     cmd_vars));

      context& ctx (*skl.ctx_);

      // This is essentially a subset of the steps we perform in b.cxx. See
      // there for more detailed comments.
      //
      scope& gs (ctx.global_scope.rw ());

      const meta_operation_info& mif (mo_perform);
      const operation_info& oif (op_update);

      ctx.current_mname = mif.name;
      ctx.current_oname = oif.name;

      gs.assign (ctx.var_build_meta_operation) = ctx.current_mname;

      // Use the build mode to signal this is a package skeleton load.
      //
      gs.assign (*ctx.var_pool.find ("build.mode")) = "skeleton";

      // Note that it's ok for out_root to not exist (external package).
      //
      const dir_path& src_root (skl.src_root_);
      const dir_path& out_root (skl.out_root_.empty ()
                                ? skl.src_root_
                                : skl.out_root_);

      auto rsi (create_root (ctx, out_root, src_root));
      scope& rs (*rsi->second.front ());

      // Note: we know this project hasn't yet been bootstrapped.
      //
      optional<bool> altn;
      value& v (bootstrap_out (rs, altn));

      if (!v)
        v = src_root;
      else
        assert (cast<dir_path> (v) == src_root);

      setup_root (rs, false /* forwarded */);

      bootstrap_pre (rs, altn);
      bootstrap_src (rs, altn,
                     skl.db_->config.relative (out_root) /* amalgamation */,
                     false                               /* subprojects */);

      create_bootstrap_outer (rs);
      bootstrap_post (rs);

      assert (mif.meta_operation_pre == nullptr);
      ctx.current_meta_operation (mif);

      ctx.enter_project_overrides (rs, out_root, ctx.var_overrides);

      return rsi;
    }
    catch (const build2::failed&)
    {
      throw failed (); // Assume the diagnostics has already been issued.
    }
  }

  const strings& package_skeleton::
  merge_cmd_vars ()
  {
    // Merge variable overrides (note that the order is important).
    //
    // We can reasonably assume reflect cannot have global or absolute scope
    // variable overrides so we don't need to pass them to context.
    //
    const strings* r;

    const strings& v1 (build2_cmd_vars);
    const strings& v2 (config_vars_);

    r = (v2.empty () ? &v1 : v1.empty () ? &v2 : nullptr);

    if (r == nullptr)
    {
      if (cmd_vars_.empty ()) // Cached.
      {
        cmd_vars_.reserve (v1.size () + v2.size ());
        cmd_vars_.assign (v1.begin (), v1.end ());
        cmd_vars_.insert (cmd_vars_.end (), v2.begin (), v2.end ());
      }

      r = &cmd_vars_;
    }

    return *r;
  }

  build2::scope& package_skeleton::
  load ()
  {
    if (ctx_ != nullptr)
      return *rs_;

    try
    {
      using namespace build2;

      auto rsi (bootstrap (*this, merge_cmd_vars ()));
      scope& rs (*rsi->second.front ());

      // Load project's root.build as well as potentially accumulated reflect
      // fragment.
      //
      // If we have the accumulated reflect fragment, wedge it just before
      // loading root.build (but after initializing config which may load
      // config.build and which we wish to override).
      //
      // Note that the plan for non-external packages is to extract the
      // configuration and then load it with config.config.load and this
      // approach should work for that case too.
      //
      function<void (parser&)> pre;

      if (!reflect_frag_.empty ())
      {
        pre = [this, &rs] (parser& p)
        {
          istringstream is (reflect_frag_);
          is.exceptions (istringstream::failbit | istringstream::badbit);

          // Note that the fragment is just a bunch of variable assignments
          // and thus unlikely to cause any errors.
          //
          path_name in ("<accumulated-reflect-fragment>");
          p.parse_buildfile (is, in, &rs, rs);
        };
      }

      load_root (rs, pre);

      setup_base (rsi,
                  out_root_.empty () ? src_root_ : out_root_,
                  src_root_);

      rs_ = &rs;
      return rs;
    }
    catch (const build2::failed&)
    {
      throw failed (); // Assume the diagnostics has already been issued.
    }
  }

  void package_skeleton::
  merge_old_config_vars ()
  {
    if (config_srcs_ == nullptr)
      return;

    assert (reflect_frag_.empty ()); // Too late.

    ctx_ = nullptr; // Reload.

    try
    {
      using namespace build2;

      scope& rs (*bootstrap (*this, merge_cmd_vars ())->second.front ());

      // Load project's root.build.
      //
      load_root (rs);

      // Extract and merge old user configuration variables from config.build
      // (or equivalent) into config_vars. Then invalidate loaded state in
      // order to make them overrides.
      //
      auto i (config_vars_.begin ()); // Insert position, see below.

      names storage;
      for (const config_variable& v: *config_srcs_)
      {
        if (v.source != config_source::user)
          continue;

        using config::variable_origin;

        pair<variable_origin, lookup> ol (config::origin (rs, v.name));

        switch (ol.first)
        {
        case variable_origin::override_:
          {
            // Already in config_vars.
            //
            // @@ TODO: theoretically, this could be an append/prepend
            //    override(s) and to make this work correctly we would need
            //    to replace them with an assign override with the final
            //    value. Maybe one day.
            //
            break;
          }
        case variable_origin::buildfile:
          {
            // Doesn't really matter where we add them though conceptually
            // feels like old should go before new (and in the original
            // order).
            //
            i = config_vars_.insert (
              i,
              serialize_cmdline (v.name, *ol.second, storage)) + 1;

            break;
          }
        case variable_origin::undefined:
        case variable_origin::default_:
          {
            // Old user configuration no longer in config.build. We could
            // complain but that feels overly drastic. Seeing that we will
            // recalculate the new set of config variable sources, let's
            // just ignore this (we could issue a warning, but who knows how
            // many times it will be issued with all this backtracking).
            //
            break;
          }
        }
      }

      config_srcs_ = nullptr; // Merged.
      cmd_vars_.clear ();     // Invalidated.
      ctx_ = nullptr;         // Drop.
    }
    catch (const build2::failed&)
    {
      throw failed (); // Assume the diagnostics has already been issued.
    }
  }
}
