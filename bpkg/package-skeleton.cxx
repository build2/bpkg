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
}

namespace bpkg
{
  // Check whether the specified configuration variable override has a
  // project variables (i.e., its name starts with config.<project>).
  //
  // Note that some user-specified variables may have qualifications
  // (global, scope, etc) but there is no reason to expect any project
  // configuration variables to use such qualifications (since they can
  // only apply to one project). So we ignore all qualified variables.
  //
  static inline bool
  project_override (const string& v, const string& p)
  {
    size_t n (p.size ());
    return v.compare (0, n, p) == 0 && strchr (".=+ \t", v[n]) != nullptr;
  }

  // Check whether the specified configuration variable name is a project
  // variables (i.e., its name starts with config.<project>).
  //
  static inline bool
  project_variable (const string& v, const string& p)
  {
    size_t n (p.size ());
    return v.compare (0, n, p) == 0 && (v[n] == '.' || v[n] == '\0');
  }

  // Customized buildfile parser that is used to detect and diagnose
  // references to undefined dependency configuration variables.
  //
  class buildfile_parser: public build2::parser
  {
  public:
    buildfile_parser (build2::context& ctx,
                      const strings& dvps,
                      optional<size_t> dvps_pending = {})
        : parser (ctx),
          dependency_var_prefixes_ (dvps),
          dependency_var_prefixes_pending_ (dvps_pending) {}

  protected:
    virtual build2::lookup
    lookup_variable (build2::name&& qual,
                     string&& name,
                     const build2::location& loc) override
    {
      using namespace build2;
      using build2::fail;
      using build2::info;

      // To avoid making copies of the name, pre-check if it is from one
      // of the dependencies.
      //
      optional<string> dep;
      if (!pre_parse_ && qual.empty ())
      {
        auto b (dependency_var_prefixes_.begin ());
        auto e (dependency_var_prefixes_pending_
                ? b + *dependency_var_prefixes_pending_
                : dependency_var_prefixes_.end ());

        if (find_if (b, e,
                     [&name] (const string& p)
                     {
                       return project_variable (name, p);
                     }) != e)
          dep = name;
      }

      lookup l (parser::lookup_variable (move (qual), move (name), loc));

      if (dep && !l.defined ())
        fail (loc) << "undefined dependency configuration variable " << *dep <<
          info << "was " << *dep << " set in earlier prefer or require clause?";

      return l;
    }

  private:
    const strings& dependency_var_prefixes_;
    optional<size_t> dependency_var_prefixes_pending_;
  };

  // Note: cannot be package_skeleton member function due to iterator return
  // (build2 stuff is only forward-declared in the header).
  //
  static build2::scope_map::iterator
  bootstrap (package_skeleton&, const strings&);

  package_skeleton::
  ~package_skeleton ()
  {
  }

  package_skeleton::
  package_skeleton (package_skeleton&& v)
      : key (move (v.key)),
        available (v.available),
        co_ (v.co_),
        db_ (v.db_),
        var_prefix_ (move (v.var_prefix_)),
        config_vars_ (move (v.config_vars_)),
        disfigure_ (v.disfigure_),
        config_srcs_ (v.config_srcs_),
        src_root_ (move (v.src_root_)),
        out_root_ (move (v.out_root_)),
        created_ (v.created_),
        verified_ (v.verified_),
        ctx_ (move (v.ctx_)),
        rs_ (v.rs_),
        cmd_vars_ (move (v.cmd_vars_)),
        cmd_vars_cache_ (v.cmd_vars_cache_),
        dependent_vars_ (move (v.dependent_vars_)),
        dependent_orgs_ (move (v.dependent_orgs_)),
        reflect_vars_ (move (v.reflect_vars_)),
        reflect_frag_ (move (v.reflect_frag_)),
        dependency_reflect_ (move (v.dependency_reflect_)),
        dependency_reflect_index_ (v.dependency_reflect_index_),
        dependency_reflect_pending_ (v.dependency_reflect_pending_),
        dependency_var_prefixes_ (move (v.dependency_var_prefixes_)),
        dependency_var_prefixes_pending_ (v.dependency_var_prefixes_pending_),
        prefer_accept_ (v.prefer_accept_)
  {
    v.db_ = nullptr;
  }

  package_skeleton& package_skeleton::
  operator= (package_skeleton&& v)
  {
    if (this != &v)
    {
      key = move (v.key);
      available = v.available;
      co_ = v.co_;
      db_ = v.db_;
      var_prefix_ = move (v.var_prefix_);
      config_vars_ = move (v.config_vars_);
      disfigure_ = v.disfigure_;
      config_srcs_ = v.config_srcs_;
      src_root_ = move (v.src_root_);
      out_root_ = move (v.out_root_);
      created_ = v.created_;
      verified_ = v.verified_;
      ctx_ = move (v.ctx_);
      rs_ = v.rs_;
      cmd_vars_ = move (v.cmd_vars_);
      cmd_vars_cache_ = v.cmd_vars_cache_;
      dependent_vars_ = move (v.dependent_vars_);
      dependent_orgs_ = move (v.dependent_orgs_);
      reflect_vars_ = move (v.reflect_vars_);
      reflect_frag_ = move (v.reflect_frag_);
      dependency_reflect_ = move (v.dependency_reflect_);
      dependency_reflect_index_ = v.dependency_reflect_index_;
      dependency_reflect_pending_ = v.dependency_reflect_pending_;
      dependency_var_prefixes_ = move (v.dependency_var_prefixes_);
      dependency_var_prefixes_pending_ = v.dependency_var_prefixes_pending_;
      prefer_accept_ = v.prefer_accept_;

      v.db_ = nullptr;
    }

    return *this;
  }

  package_skeleton::
  package_skeleton (const package_skeleton& v)
      : key (v.key),
        available (v.available),
        co_ (v.co_),
        db_ (v.db_),
        var_prefix_ (v.var_prefix_),
        config_vars_ (v.config_vars_),
        disfigure_ (v.disfigure_),
        config_srcs_ (v.config_srcs_),
        src_root_ (v.src_root_),
        out_root_ (v.out_root_),
        created_ (v.created_),
        verified_ (v.verified_),
        cmd_vars_ (v.cmd_vars_),
        cmd_vars_cache_ (v.cmd_vars_cache_),
        dependent_vars_ (v.dependent_vars_),
        dependent_orgs_ (v.dependent_orgs_),
        reflect_vars_ (v.reflect_vars_),
        reflect_frag_ (v.reflect_frag_),
        dependency_reflect_ (v.dependency_reflect_),
        dependency_reflect_index_ (v.dependency_reflect_index_),
        dependency_reflect_pending_ (v.dependency_reflect_pending_),
        dependency_var_prefixes_ (v.dependency_var_prefixes_),
        dependency_var_prefixes_pending_ (v.dependency_var_prefixes_pending_),
        prefer_accept_ (v.prefer_accept_)
  {
    // The idea here is to create an "unloaded" copy but with enough state
    // that it can be loaded if necessary.
    //
    // Note that there is a bit of a hole in this logic with regards to the
    // prefer_accept_ semantics but it looks like we cannot plausible trigger
    // it (which is fortified with an assert in evaluate_reflect(); note that
    // doing it here would be overly strict since this may have a left-over
    // prefer_accept_ position).
  }

  void package_skeleton::
  reset ()
  {
    assert (db_ != nullptr); // Cannot be called after collect_config().

    rs_ = nullptr;
    ctx_ = nullptr; // Free.

    cmd_vars_.clear ();
    cmd_vars_cache_ = false;

    dependent_vars_.clear ();
    dependent_orgs_.clear ();

    reflect_vars_.clear ();
    reflect_frag_.clear ();

    dependency_reflect_.clear ();
    dependency_reflect_index_ = 0;
    dependency_reflect_pending_ = 0;

    dependency_var_prefixes_.clear ();
    dependency_var_prefixes_pending_ = 0;

    prefer_accept_ = nullopt;
  }

  package_skeleton::
  package_skeleton (const common_options& co,
                    database& db,
                    shared_ptr<const available_package> ap,
                    strings cvs,
                    bool df,
                    const vector<config_variable>* css,
                    optional<dir_path> src_root,
                    optional<dir_path> out_root)
      : key (db, ap->id.name),
        available (move (ap)),
        co_ (&co),
        db_ (&db),
        var_prefix_ ("config." + key.name.variable ()),
        config_vars_ (move (cvs)),
        disfigure_ (df),
        config_srcs_ (df ? nullptr : css)
  {
    // Should not be created for stubs.
    //
    assert (available != nullptr && available->bootstrap_build);

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

  // Reverse value to names.
  //
  static optional<build2::names>
  reverse_value (const build2::value& val)
  {
    using namespace build2;

    if (val.null)
      return nullopt;

    names storage;
    names_view nv (reverse (val, storage));

    return (nv.data () == storage.data ()
            ? move (storage)
            : names (nv.begin (), nv.end ()));
  }

  // Return the dependent (origin==buildfile) configuration variables as
  // command line overrides. If the second argument is not NULL, then populate
  // it with the corresponding originating dependents.
  //
  static strings
  dependent_cmd_vars (const package_configuration& cfg,
                      vector<package_key>* orgs = nullptr)
  {
    using build2::config::variable_origin;

    strings r;

    for (const config_variable_value& v: cfg)
    {
      if (v.origin == variable_origin::buildfile)
      {
        r.push_back (v.serialize_cmdline ());

        if (orgs != nullptr)
          orgs->push_back (*v.dependent);
      }
    }

    return r;
  }

  void package_skeleton::
  reload_defaults (package_configuration& cfg)
  {
    // Should only be called before dependent_config()/evaluate_*().
    //
    assert (dependent_vars_.empty ()     &&
            reflect_vars_.empty ()       &&
            dependency_reflect_.empty () &&
            ctx_ == nullptr);

    if (config_srcs_ != nullptr)
      load_old_config ();

    try
    {
      using namespace build2;

      // This is what needs to happen to the variables of different origins in
      // the passed configuration:
      //
      // default              -- reloaded
      // buildfile/dependent  -- made command line override
      // override/user        -- should match what's in config_vars_
      // undefined            -- reloaded
      //
      // Note also that on the first call we will have no configuration. And
      // so to keep things simple, we merge variable of the buildfile origin
      // into cmd_vars and then rebuild things from scratch. Note, however,
      // that below we need to sort out these merged overrides into user and
      // dependent, so we keep the old configuration for reference.
      //
      // Note also that dependent values do not clash with user overrides by
      // construction (in evaluate_{prefer_accept,require}()): we do not add
      // as dependent variables that have the override origin.
      //
      scope& rs (
        *bootstrap (
          *this, merge_cmd_vars (dependent_cmd_vars (cfg)))->second.front ());

      // Load project's root.build.
      //
      load_root (rs);

      package_configuration old (move (cfg));
      cfg.package = move (old.package);

      // Note that a configuration variable may not have a default value so we
      // cannot just iterate over all the config.<name>** values set on the
      // root scope. Our options seem to be either iterating over the variable
      // pool or forcing the config module with config.config.module=true and
      // then using its saved variables map. Since the amount of stuff we load
      // is quite limited, there shouldn't be too many variables in the pool.
      // So let's go with the simpler approach for now.
      //
      // Though the saved variables map approach would have been more accurate
      // since that's the variables that were introduced with the config
      // directive. Potentially the user could just have a buildfile
      // config.<name>** variable but it feels like that should be harmless
      // (we will return it but nobody will presumably use that information).
      // Also, if/when we start tracking the configuration variable
      // dependencies (i.e., which default value depend on which config
      // variable), then the saved variables map seem like the natural place
      // to keep this information.
      //
      // @@ One potentially-bogus config variable could be config.*.develop.
      //    Would have been nice not to drag it around if not used by the
      //    package. And, could be helpful to warn that configuration variable
      //    does not exist. But we cannot do it consistently since we don't
      //    always load the skeleton.
      //
      for (const variable& var: rs.ctx.var_pool)
      {
        if (!project_variable (var.name, var_prefix_))
          continue;

        using config::variable_origin;

        pair<variable_origin, lookup> ol (config::origin (rs, var));

        switch (ol.first)
        {
        case variable_origin::default_:
        case variable_origin::override_:
        case variable_origin::undefined:
          {
            config_variable_value v {var.name, ol.first, {}, {}, {}, false};

            // Override could mean user override from config_vars_ or the
            // dependent override that we have merged above.
            //
            if (v.origin == variable_origin::override_)
            {
              if (config_variable_value* ov = old.find (v.name))
              {
                if (ov->origin == variable_origin::buildfile)
                {
                  v.origin = variable_origin::buildfile;
                  v.dependent = move (ov->dependent);
                  v.confirmed = ov->confirmed;
                }
                else
                  assert (ov->origin == variable_origin::override_);
              }
            }

            // Save value.
            //
            if (v.origin != variable_origin::undefined)
              v.value = reverse_value (*ol.second);

            // Save type.
            //
            if (var.type != nullptr)
              v.type = var.type->name;

            cfg.push_back (move (v));
            break;
          }
        case variable_origin::buildfile:
          {
            // Feel like this shouldn't happen since we have disfigured them.
            //
            assert (false);
            break;
          }
        }
      }

      verified_ = true; // Managed to load without errors.
      ctx_ = nullptr;
    }
    catch (const build2::failed&)
    {
      throw failed (); // Assume the diagnostics has already been issued.
    }
  }

  pair<bool, string> package_skeleton::
  verify_sensible (const package_configuration& cfg)
  {
    // Should only be called before dependent_config()/evaluate_*().
    //
    assert (dependent_vars_.empty ()     &&
            reflect_vars_.empty ()       &&
            dependency_reflect_.empty () &&
            ctx_ == nullptr);

    if (config_srcs_ != nullptr)
      load_old_config ();

    try
    {
      using namespace build2;

      // For now we treat any failure to load root.build as bad configuration,
      // which is not very precise. One idea to make this more precise would
      // be to invent some kind of tagging for "bad configuration" diagnostics
      // (e.g., either via an attribute or via special config.assert directive
      // or some such).
      //
      // For now we rely on load_defaults() and load_old_config() to "flush"
      // out any unrelated errors (e.g., one of the modules configuration is
      // bad, etc). However, if that did not happen naturally, then we must do
      // it ourselves.
      //
      if (!verified_)
      {
        scope& rs (
          *bootstrap (*this, merge_cmd_vars (strings {}))->second.front ());
        load_root (rs);

        verified_ = true;
        ctx_ = nullptr;
      }

      scope& rs (
        *bootstrap (
          *this, merge_cmd_vars (dependent_cmd_vars (cfg)))->second.front ());

      // Load project's root.build while redirecting the diagnostics stream.
      //
      ostringstream ds;
      auto dg (make_guard ([ods = diag_stream] () {diag_stream = ods;}));
      diag_stream = &ds;

      pair<bool, string> r;
      try
      {
        load_root (rs);
        r.first = true;
      }
      catch (const build2::failed&)
      {
        r.first = false;
        r.second = trim (ds.str ());
      }

      ctx_ = nullptr;
      return r;
    }
    catch (const build2::failed&)
    {
      throw failed (); // Assume the diagnostics has already been issued.
    }
  }

  void package_skeleton::
  dependent_config (const package_configuration& cfg)
  {
    assert (dependent_vars_.empty ()); // Must be called at most once.

    dependent_vars_ = dependent_cmd_vars (cfg, &dependent_orgs_);
  }

  // Print the location of a depends value in the specified manifest file.
  //
  // Note that currently we only use this function for the external packages.
  // We could also do something similar for normal packages by pointing to the
  // manifest we have serialized. In this case we would also need to make sure
  // the temp directory is not cleaned in case of an error. Maybe one day.
  //
  static void
  depends_location (const build2::diag_record& dr,
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
            dr << build2::info (build2::location (mf,
                                                  nv.value_line,
                                                  nv.value_column))
               << "in depends manifest value defined here";
            break;
          }
        }
      }
    }
    catch (const manifest_parsing&) {}
    catch (const io_error&) {}
  }

  bool package_skeleton::
  evaluate_enable (const string& cond, pair<size_t, size_t> indexes)
  {
    size_t depends_index (indexes.first);

    try
    {
      using namespace build2;
      using build2::fail;
      using build2::info;
      using build2::endf;

      // Drop the state from the previous evaluation of prefer/accept.
      //
      if (prefer_accept_)
      {
        ctx_ = nullptr;
        prefer_accept_ = nullopt;
      }

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
        [this, &cond, &rs, depends_index] (const build2::diag_record& dr)
        {
          dr << info << "enable condition: (" << cond << ")";

          // For external packages we have the manifest so print the location
          // of the depends value in questions.
          //
          if (rs.out_eq_src ())
            dr << info << "in depends manifest value of package " << key.name;
          else
            depends_location (dr,
                              rs.src_path () / manifest_file,
                              depends_index);
        });

      lexer l (is, in, il /* start line */);
      buildfile_parser p (rs.ctx, dependency_var_prefixes_);
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

  void package_skeleton::
  evaluate_reflect (const string& refl, pair<size_t, size_t> indexes)
  {
    size_t depends_index (indexes.first);

    // The reflect configuration variables are essentially overrides that will
    // be passed on the command line when we configure the package. They could
    // clash with configuration variables specified by the user (config_vars_)
    // and it feels like user values should take precedence. Though one could
    // also argue we should diagnose this case and fail not to cause more
    // confusion.
    //
    // @@ They could also clash with dependent configuration. Probably should
    //    be handled in the same way (it's just another type of "user"). Yes,
    //    since dependent_vars_ are entered as cmd line overrides, this is
    //    how they are treated (but may need to adjust the diagnostics).
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
    // reflect clauses (so that they can use the previously reflected values
    // or values that are derived from them in root.build). It seems like we
    // have two options here: either enter them as true overrides similar to
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
    // we do this merging is unclear. Hm, but they are config vars...
    //
    try
    {
      // Note: similar in many respects to evaluate_enable().
      //
      using namespace build2;
      using build2::fail;
      using build2::info;
      using build2::endf;

      // Drop the state from the previous evaluation of prefer/accept if it's
      // from the wrong position.
      //
      optional<size_t> dependency_var_prefixes_pending;
      if (prefer_accept_)
      {
        if (*prefer_accept_ != indexes)
        {
          ctx_ = nullptr;
          prefer_accept_ = nullopt;
        }
        else
        {
          // This could theoretically happen if we make a copy of the skeleton
          // after evaluate_prefer_accept() and then attempt to continue with
          // the call on the copy to evaluate_reflect() passing the same
          // position. But it doesn't appear our usage should trigger this.
          //
          assert (ctx_ != nullptr);

          dependency_var_prefixes_pending = dependency_var_prefixes_pending_;
        }
      }

      scope& rs (load ());

      istringstream is (refl);
      is.exceptions (istringstream::failbit | istringstream::badbit);

      path_name in ("<depends-reflect-clause>");
      uint64_t il (1);

      auto df = build2::make_diag_frame (
        [this, &refl, &rs, depends_index] (const build2::diag_record& dr)
        {
          // Probably safe to assume a one-line fragment contains a variable
          // assignment.
          //
          if (refl.find ('\n') == string::npos)
            dr << info << "reflect variable: " << trim (string (refl));
          else
            dr << info << "reflect clause:\n"
               << trim_right (string (refl));

          // For external packages we have the manifest so print the location
          // of the depends value in questions.
          //
          if (rs.out_eq_src ())
            dr << info << "in depends manifest value of package " << key.name;
          else
            depends_location (dr,
                              rs.src_path () / manifest_file,
                              depends_index);
        });

      // Note: a lot of this code is inspired by the config module.
      //

      // Collect all the set config.<name>.* variables on the first pass and
      // filter out unchanged on the second.
      //
      auto& vp (rs.var_pool ());
      const string& ns (var_prefix_);

      struct value_data
      {
        const value* val;
        size_t       ver;
      };

      // @@ Maybe redo as small_vector?
      //
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
      buildfile_parser p (rs.ctx,
                          dependency_var_prefixes_,
                          dependency_var_prefixes_pending);
      p.parse_buildfile (l, &rs, rs);

      process (false);

      // Add to the map the reflect variables collected previously. Note that
      // we can re-purpose the override since we re-populate it.
      //
      for (string& n: reflect_vars_)
      {
        // Transform `name=value` to just `name`.
        //
        {
          size_t p (n.find ('='));
          assert (p != string::npos); // We've serialized it so must be there.
          n.resize (p);
        }

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
      reflect_vars_.clear ();
      reflect_frag_.clear ();

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

        // For the accumulated fragment we always save the original and let
        // the standard overriding take its course.
        //
        serialize_buildfile (reflect_frag_, var.name, val, storage);

        // Note: this is currently disabled and is likely very outdated.
        //
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

        // @@ Can't we redo it via origin() call like in other places?
        //
        pair<lookup, size_t> org {lookup {val, var, rs.vars}, 1 /* depth */};
        pair<lookup, size_t> ovr;

        if (var.overrides == nullptr)
          ovr = org; // Case #2.
        else
        {
          // NOTE: see also below if enabling this.
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
          // @@ TODO: probably also depends, not just user.

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

  bool package_skeleton::
  evaluate_prefer_accept (const dependency_configurations& cfgs,
                          const string& prefer,
                          const string& accept,
                          pair<size_t, size_t> indexes)
  {
    size_t depends_index (indexes.first);

    assert (dependency_reflect_index_ <= depends_index);

    try
    {
      using namespace build2;
      using config::variable_origin;
      using build2::fail;
      using build2::info;
      using build2::endf;

      // Drop the state from the previous evaluation of prefer/accept.
      //
      if (prefer_accept_)
      {
        ctx_ = nullptr;
        prefer_accept_ = nullopt;
      }

      // Drop any dependency reflect values from the previous evaluation of
      // this clause, if any.
      //
      if (dependency_reflect_index_ == depends_index)
        dependency_reflect_.resize (dependency_reflect_pending_);

      // This is what needs to happen to the variables of different origins in
      // the passed dependency configurations:
      //
      // default              -- set as default (value::extra=1)
      // buildfile/dependent  -- set as buildfile (value::extra=2)
      // override/user        -- set as override (so cannot be overriden)
      // undefined            -- ignored
      //
      // Note that we set value::extra to 2 for buildfile/dependent values.
      // This is done so that we can detect when they were set by this
      // dependent (even if to the same value). Note that the build2 config
      // module only treats 1 as the default value marker.
      //
      // Additionally, for all origins we need to typify the variables.
      //
      // All of this is done by load(), including removing and returning the
      // dependency variable prefixes (config.<project>) which we later add
      // to dependency_var_prefixes_.
      //
      strings dvps;
      scope& rs (load (cfgs, &dvps, true /* defaults */));

      // Evaluate the prefer clause.
      //
      {
        istringstream is (prefer);
        is.exceptions (istringstream::failbit | istringstream::badbit);

        path_name in ("<depends-prefer-clause>");
        uint64_t il (1);

        auto df = build2::make_diag_frame (
          [this, &prefer, &rs, depends_index] (const build2::diag_record& dr)
          {
            dr << info << "prefer clause:\n"
               << trim_right (string (prefer));

            // For external packages we have the manifest so print the
            // location of the depends value in questions.
            //
            if (rs.out_eq_src ())
              dr << info << "in depends manifest value of package " << key.name;
            else
              depends_location (dr,
                                rs.src_path () / manifest_file,
                                depends_index);
          });

        lexer l (is, in, il /* start line */);
        buildfile_parser p (rs.ctx, dependency_var_prefixes_);
        p.parse_buildfile (l, &rs, rs);

        // Check if the dependent set any stray configuration variables.
        //
        for (size_t i (0); i != cfgs.size (); ++i)
        {
          package_configuration& cfg (cfgs[i]);

          const string& ns (dvps[i]); // Parallel.
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

            if (cfg.find (var.name) == nullptr)
            {
              fail << "package " << cfg.package.name << " has no "
                   << "configuration variable " << var.name <<
                info << var.name << " set in require clause of dependent "
                   << key.string ();
            }
          }
        }
      }

      // Evaluate the accept clause.
      //
      bool r;
      {
        istringstream is ('(' + accept + ')');
        is.exceptions (istringstream::failbit | istringstream::badbit);

        path_name in ("<depends-accept-clause>");
        uint64_t il (1);

        auto df = build2::make_diag_frame (
          [this, &accept, &rs, depends_index] (const build2::diag_record& dr)
          {
            dr << info << "accept condition: (" << accept << ")";

            // For external packages we have the manifest so print the
            // location of the depends value in questions.
            //
            if (rs.out_eq_src ())
              dr << info << "in depends manifest value of package " << key.name;
            else
              depends_location (dr,
                                rs.src_path () / manifest_file,
                                depends_index);
          });

        lexer l (is, in, il /* start line */);
        buildfile_parser p (rs.ctx, dependency_var_prefixes_);
        value v (p.parse_eval (l, rs, rs, parser::pattern_mode::expand));

        try
        {
          // Should evaluate to 'true' or 'false'.
          //
          r = build2::convert<bool> (move (v));
        }
        catch (const invalid_argument& e)
        {
          fail (build2::location (in, il)) << e << endf;
        }
      }

      // If acceptable, update the configuration with the new values, if any.
      //
      // We also save the subset of values that were set by this dependent to
      // be reflected to further clauses.
      //
      if (r)
      {
        dependency_reflect_index_ = depends_index;
        dependency_reflect_pending_ = dependency_reflect_.size ();

        for (size_t i (0); i != cfgs.size (); ++i)
        {
          package_configuration& cfg (cfgs[i]);

          const string& ns (dvps[i]);
          for (auto p (rs.vars.lookup_namespace (ns));
               p.first != p.second;
               ++p.first)
          {
            const variable& var (p.first->first);

            if (var.override ())
              continue;

            const value& val (p.first->second);

            pair<variable_origin, lookup> ol (
              config::origin (rs,
                              var,
                              pair<lookup, size_t> {
                                lookup {val, var, rs.vars}, 1 /* depth */}));

            config_variable_value& v (*cfg.find (var.name));

            // An override cannot become a non-override. And a non-override
            // cannot become an override. Except that the dependency override
            // could be specified (only) for the dependent.
            //
            if (v.origin == variable_origin::override_)
            {
              assert (ol.first == variable_origin::override_);
            }
            else if (ol.first == variable_origin::override_ &&
                     v.origin != variable_origin::override_)
            {
              fail << "dependency override " << var.name << " specified for "
                   << "dependent " << key.string () << " but not dependency" <<
                info << "did you mean to specify ?" << cfg.package.name
                   << " +{ " << var.name << "=... }";
            }

            switch (ol.first)
            {
            case variable_origin::buildfile:
              {
                optional<names> ns (reverse_value (val));

                // If this value was set, save it as a dependency reflect.
                //
                if (val.extra == 0)
                {
                  dependency_reflect_.push_back (
                    reflect_variable_value {v.name, ol.first, v.type, ns});
                }

                // Possible transitions:
                //
                // default/undefine -> buildfile -- override dependency default
                // buildfile        -> buildfile -- override other dependent
                //
                if (v.origin == variable_origin::buildfile)
                {
                  // If unchanged, then we keep the old originating dependent
                  // (even if the value was technically "overwritten" by this
                  // dependent).
                  //
                  if (val.extra == 2 || v.value == ns)
                    break;
                }
                else
                  v.origin = variable_origin::buildfile;

                v.value = move (ns);
                v.dependent = key; // We are the originating dependent.
                v.confirmed = true;
                break;
              }
            case variable_origin::default_:
              {
                // A default can only come from a default.
                //
                assert (ol.first == v.origin);
                break;
              }
            case variable_origin::override_:
              {
                // If the value was set by this dependent then we need to
                // reflect it even if it was overridden (but as the overridden
                // value). Note that the mere presence of the value in rs.vars
                // is not enough to say that it was set -- it could also be
                // the default value. But we can detect that by examining
                // value::extra.
                //
                if (val.extra == 0)
                {
                  dependency_reflect_.push_back (
                    reflect_variable_value {
                      v.name, ol.first, v.type, reverse_value (*ol.second)});
                }
                break;
              }
            case variable_origin::undefined:
              {
                // Not possible since we have the defined original.
                //
                assert (false);
                break;
              }
            }
          }
        }

        // Note that because we add it here, the following reflect clause will
        // not be able to expand undefined values. We handle this by keeping a
        // pending position.
        //
        dependency_var_prefixes_pending_ = dependency_var_prefixes_.size ();
        dependency_var_prefixes_.insert (dependency_var_prefixes_.end (),
                                         make_move_iterator (dvps.begin ()),
                                         make_move_iterator (dvps.end ()));

        // Note: do not drop the build system state yet since it should be
        // reused by the following reflect clause, if any.
        //
        prefer_accept_ = indexes;
      }
      else
        ctx_ = nullptr;

      return r;
    }
    catch (const build2::failed&)
    {
      throw failed (); // Assume the diagnostics has already been issued.
    }
  }

  bool package_skeleton::
  evaluate_require (const dependency_configurations& cfgs,
                    const string& require, pair<size_t, size_t> indexes)
  {
    size_t depends_index (indexes.first);

    assert (dependency_reflect_index_ <= depends_index);

    try
    {
      using namespace build2;
      using config::variable_origin;
      using build2::fail;
      using build2::info;
      using build2::endf;

      // Drop the state from the previous evaluation of prefer/accept.
      //
      if (prefer_accept_)
      {
        ctx_ = nullptr;
        prefer_accept_ = nullopt;
      }

      // Drop any dependency reflect values from the previous evaluation of
      // this clause, if any.
      //
      if (dependency_reflect_index_ == depends_index)
        dependency_reflect_.resize (dependency_reflect_pending_);

      // A require clause can only set bool configuration variables and only
      // to true and may not have any conditions on other configuration
      // variables (including their origin). As a result, we don't need to set
      // the default (or other dependent) values, but will need the type
      // information as well as overrides (see negotiate_configuration()
      // for details).
      //
      strings dvps;
      scope& rs (load (cfgs, &dvps, false /* defaults */));

      // Evaluate the require clause.
      //
      {
        istringstream is (require);
        is.exceptions (istringstream::failbit | istringstream::badbit);

        path_name in ("<depends-require-clause>");
        uint64_t il (1);

        auto df = build2::make_diag_frame (
          [this, &require, &rs, depends_index] (const build2::diag_record& dr)
          {
            dr << info << "require clause:\n"
               << trim_right (string (require));

            // For external packages we have the manifest so print the
            // location of the depends value in questions.
            //
            if (rs.out_eq_src ())
              dr << info << "in depends manifest value of package " << key.name;
            else
              depends_location (dr,
                                rs.src_path () / manifest_file,
                                depends_index);
          });

        lexer l (is, in, il /* start line */);
        buildfile_parser p (rs.ctx, dependency_var_prefixes_);
        p.parse_buildfile (l, &rs, rs);

        // Check for stray variables and enforce all the require restrictions
        // (bool, set to true, etc).
        //
        for (size_t i (0); i != cfgs.size (); ++i)
        {
          package_configuration& cfg (cfgs[i]);

          const string& ns (dvps[i]); // Parallel.
          for (auto p (rs.vars.lookup_namespace (ns));
               p.first != p.second;
               ++p.first)
          {
            // Note that because we didn't set any default (or other
            // dependent) values, all the values we see are set by this
            // dependent.
            //
            const variable& var (p.first->first);

            // This can be one of the overrides (__override, __prefix, etc),
            // which we skip.
            //
            if (var.override ())
              continue;

            const config_variable_value* v (cfg.find (var.name));

            if (v == nullptr)
            {
              fail << "package " << cfg.package.name << " has no configuration "
                   << "variable " << var.name <<
                info << var.name << " set in require clause of dependent "
                   << key.string ();
            }

            if (!v->type || *v->type != "bool")
            {
              fail << "configuration variable " << var.name << " is not of "
                   << "bool type" <<
                info << var.name << " set in require clause of dependent "
                   << key.string ();
            }

            const value& val (p.first->second);

            if (!cast_false<bool> (val))
            {
              fail << "configuration variable " << var.name << " is not set "
                   << "to true" <<
                info << var.name << " set in require clause of dependent "
                   << key.string ();
            }
          }
        }
      }

      // First determine if acceptable.
      //
      bool r (true);
      for (size_t i (0); i != cfgs.size (); ++i)
      {
        package_configuration& cfg (cfgs[i]);

        const string& ns (dvps[i]);
        for (auto p (rs.vars.lookup_namespace (ns));
             p.first != p.second;
             ++p.first)
        {
          const variable& var (p.first->first);

          if (var.override ())
            continue;

          const value& val (p.first->second);

          const config_variable_value& v (*cfg.find (var.name));

          // The only situation where the result would not be acceptable is if
          // one of the values were overridden to false.
          //
          pair<variable_origin, lookup> ol (
            config::origin (rs,
                            var,
                            pair<lookup, size_t> {
                              lookup {val, var, rs.vars}, 1 /* depth */}));

          // An override cannot become a non-override. And a non-override
          // cannot become an override. Except that the dependency override
          // could be specified (only) for the dependent.
          //
          if (v.origin == variable_origin::override_)
          {
            assert (ol.first == variable_origin::override_);
          }
          else if (ol.first == variable_origin::override_ &&
                   v.origin != variable_origin::override_)
          {
            fail << "dependency override " << var.name << " specified for "
                 << "dependent " << key.string () << " but not dependency" <<
              info << "did you mean to specify ?" << cfg.package.name
                 << " +{ " << var.name << "=... }";
          }

          if (ol.first == variable_origin::override_)
          {
            if (!cast_false<bool> (*ol.second))
              r = false;
          }
        }
      }

      // If acceptable, update the configuration with the new values, if any.
      //
      // Note that we cannot easily combine this loop with the above because
      // we should not modify configurations if the result is not acceptable.
      //
      // We also save the subset of values that were set by this dependent to
      // be reflected to further clauses.
      //
      if (r)
      {
        dependency_reflect_index_ = depends_index;
        dependency_reflect_pending_ = dependency_reflect_.size ();

        for (size_t i (0); i != cfgs.size (); ++i)
        {
          package_configuration& cfg (cfgs[i]);

          const string& ns (dvps[i]);
          for (auto p (rs.vars.lookup_namespace (ns));
               p.first != p.second;
               ++p.first)
          {
            const variable& var (p.first->first);

            if (var.override ())
              continue;

            config_variable_value& v (*cfg.find (var.name));

            // This value was set so save it as a dependency reflect.
            //
            // Note that unlike the equivalent evaluate_prefer_accept() logic,
            // here the value cannot be the default/buildfile (since we don't
            // set those; see the load() call above).
            //
            optional<names> ns (names {build2::name ("true")});

            dependency_reflect_.push_back (
              reflect_variable_value {v.name, v.origin, v.type, ns});

            if (v.origin != variable_origin::override_)
            {
              // Possible transitions:
              //
              // default/undefine -> buildfile -- override dependency default
              // buildfile        -> buildfile -- override other dependent
              //

              if (v.origin == variable_origin::buildfile)
              {
                // If unchanged, then we keep the old originating dependent
                // (even if the value was technically "overwritten" by this
                // dependent).
                //
                if (v.value == ns)
                  continue;
              }
              else
                v.origin = variable_origin::buildfile;

              v.value = move (ns);
              v.dependent = key; // We are the originating dependent.
              v.confirmed = true;
            }
          }
        }

        dependency_var_prefixes_.insert (dependency_var_prefixes_.end (),
                                         make_move_iterator (dvps.begin ()),
                                         make_move_iterator (dvps.end ()));
      }

      // Drop the build system state since it needs reloading (while it may
      // seem safe for us to keep the state since we didn't set any defaults,
      // we may have overrides that the clause did not set, so let's drop it
      // for good measure and also to keep things simple).
      //
      ctx_ = nullptr;

      return r;
    }
    catch (const build2::failed&)
    {
      throw failed (); // Assume the diagnostics has already been issued.
    }
  }

  bool package_skeleton::
  empty ()
  {
    if (config_srcs_ != nullptr)
      load_old_config ();

    return (dependent_vars_.empty () &&
            reflect_vars_.empty ()   &&
            find_if (config_vars_.begin (), config_vars_.end (),
                     [this] (const string& v)
                     {
                       return project_override (v, var_prefix_);
                     }) == config_vars_.end ());
  }

  void package_skeleton::
  print_config (ostream& os, const char* indent)
  {
    if (config_srcs_ != nullptr)
      load_old_config ();

    auto print = [&os,
                  indent,
                  first = true] (const string& v) mutable -> ostream&
    {
      if (first)
        first = false;
      else
        os << '\n';

      os << indent << v;
      return os;
    };

    // First comes the user configuration.
    //
    for (const string& v: config_vars_)
    {
      if (project_override (v, var_prefix_))
        print (v) << " (user configuration)";
    }

    // Next dependent configuration.
    //
    for (size_t i (0); i != dependent_vars_.size (); ++i)
    {
      const string& v (dependent_vars_[i]);
      const package_key& d (dependent_orgs_[i]); // Parallel.

      print (v) << " (set by " << d << ')';
    }

    // Finally reflect.
    //
    for (const string& v: reflect_vars_)
    {
      print (v) << " (set by " << key.name << ')';
    }
  }

  pair<strings, vector<config_variable>> package_skeleton::
  collect_config () &&
  {
    assert (db_ != nullptr); // Must be called only once.

    if (config_srcs_ != nullptr)
      load_old_config ();

    // Merge all the variables into a single list in the correct order
    // and assign their sources while at it.
    //
    strings vars;
    vector<config_variable> srcs;

    if (size_t n = (config_vars_.size () +
                    dependent_vars_.size () +
                    reflect_vars_.size ()))
    {
      // For vars we will steal the first non-empty *_vars_. But for sources
      // reserve the space.
      //
      srcs.reserve (n); // At most that many.

      // Return the variable name given the variable override.
      //
      auto var_name = [] (const string& v)
      {
        size_t p (v.find_first_of ("=+ \t"));
        assert (p != string::npos);
        return string (v, 0, p);
      };

      // Note that we assume the three sets of variables do not clash.
      //

      // First comes the user configuration.
      //
      if (!config_vars_.empty ())
      {
        // Assign the user source only to user-specified configuration
        // variables which are project variables (i.e., names start with
        // config.<project>).
        //
        for (const string& v: config_vars_)
        {
          if (project_override (v, var_prefix_))
          {
            string n (var_name (v));

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

        vars = move (config_vars_);
      }

      // Next dependent configuration.
      //
      if (!dependent_vars_.empty ())
      {
        // These are all project variables. There should also be no duplicates
        // by construction.
        //
        for (const string& v: dependent_vars_)
          srcs.push_back (
            config_variable {var_name (v), config_source::dependent});

        if (vars.empty ())
          vars = move (dependent_vars_);
        else
        {
          vars.reserve (n);
          vars.insert (vars.end (),
                       make_move_iterator (dependent_vars_.begin ()),
                       make_move_iterator (dependent_vars_.end ()));
        }
      }

      // Finally reflect.
      //
      if (!reflect_vars_.empty ())
      {
        // These are all project variables. There should also be no duplicates
        // by construction (see evaluate_reflect()).
        //
        for (const string& v: reflect_vars_)
          srcs.push_back (
            config_variable {var_name (v), config_source::reflect});

        if (vars.empty ())
          vars = move (reflect_vars_);
        else
        {
          vars.reserve (n);
          vars.insert (vars.end (),
                       make_move_iterator (reflect_vars_.begin ()),
                       make_move_iterator (reflect_vars_.end ()));
        }
      }
    }

    ctx_ = nullptr; // Free.
    db_ = nullptr;

    return make_pair (move (vars), move (srcs));
  }

  const strings& package_skeleton::
  merge_cmd_vars (const strings& dependent_vars,
                  const strings& dependency_vars,
                  bool cache)
  {
    // Merge variable overrides (note that the order is important). See also a
    // custom/optimized version in load_old_config().
    //
    if (!cache || !cmd_vars_cache_)
    {
      const strings& vs1 (build2_cmd_vars);
      const strings& vs2 (config_vars_);
      const strings& vs3 (dependent_vars);  // Should not override.
      const strings& vs4 (dependency_vars); // Should not override.

      // Try to reuse both vector and string buffers.
      //
      cmd_vars_.resize (
        1 + vs1.size () + vs2.size () + vs3.size () + vs4.size ());

      size_t i (0);
      {
        string& v (cmd_vars_[i++]);

        // If the package is being disfigured, then don't load config.build at
        // all. Otherwise, disfigure all package variables (config.<name>**).
        //
        // Note that this semantics must be consistent with how we actually
        // configure the package in pkg_configure().
        //
        if (disfigure_)
          v = "config.config.unload=true";
        else
        {
          // Note: must be quoted to preserve the pattern.
          //
          v = "config.config.disfigure='config.";
          v += name ().variable ();
          v += "**'";
        }
      }

      for (const string& v: vs1) cmd_vars_[i++] = v;
      for (const string& v: vs2) cmd_vars_[i++] = v;
      for (const string& v: vs3) cmd_vars_[i++] = v;
      for (const string& v: vs4) cmd_vars_[i++] = v;

      cmd_vars_cache_ = cache;
    }

    return cmd_vars_;
  }

  void package_skeleton::
  load_old_config ()
  {
    assert (config_srcs_ != nullptr && ctx_ == nullptr);

    try
    {
      using namespace build2;

      // This load that must be done without config.config.disfigure. Also, it
      // would be nice to optimize for the common case where the only load is
      // to get the old configuration (e.g., config.*.develop) as part of
      // collect_config(). So instead of calling merge_cmd_vars() we will do
      // own (but consistent) thing.
      //
      const strings* cmd_vars;
      {
        assert (!cmd_vars_cache_); // Sanity check (we are always first).

        const strings& vs1 (build2_cmd_vars);
        const strings& vs2 (config_vars_);

        cmd_vars = (vs2.empty () ? &vs1 : vs1.empty () ? &vs2 : nullptr);

        if (cmd_vars == nullptr)
        {
          // Note: the order is important (see merge_cmd_vars()).
          //
          cmd_vars_.reserve (vs1.size () + vs2.size ());
          cmd_vars_.assign (vs1.begin (), vs1.end ());
          cmd_vars_.insert (cmd_vars_.end (), vs2.begin (), vs2.end ());

          cmd_vars = &cmd_vars_;
        }
      }

      scope& rs (*bootstrap (*this, *cmd_vars)->second.front ());

      // Load project's root.build.
      //
      load_root (rs);

      // Extract and merge old user configuration variables from config.build
      // (or equivalent) into config_vars.
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

      config_srcs_ = nullptr;
      verified_ = true; // Managed to load without errors.
      ctx_ = nullptr;
    }
    catch (const build2::failed&)
    {
      throw failed (); // Assume the diagnostics has already been issued.
    }
  }

  build2::scope& package_skeleton::
  load (const dependency_configurations& cfgs, strings* dvps, bool defaults)
  {
    if (ctx_ != nullptr)
    {
      // We have to reload if there is any dependency configuration.
      //
      if (cfgs.empty ())
        return *rs_;

      ctx_ = nullptr;
    }

    if (config_srcs_ != nullptr)
      load_old_config ();

    try
    {
      using namespace build2;
      using build2::config::variable_origin;

      // If we have any dependency configurations, then here we need to add
      // dependency configuration variables with the override origin to the
      // command line overrides (see evaluate_prefer_accept() for details).
      // While at it, handle dependency variable prefixes.
      //
      strings dependency_vars;
      for (const package_configuration& cfg: cfgs)
      {
        for (const config_variable_value& v: cfg)
        {
          if (v.origin == variable_origin::override_)
            dependency_vars.push_back (v.serialize_cmdline ());
        }

        string p ("config." + cfg.package.name.variable ());

        auto i (find (dependency_var_prefixes_.begin (),
                      dependency_var_prefixes_.end (),
                      p));
        if (i != dependency_var_prefixes_.end ())
          dependency_var_prefixes_.erase (i);

        dvps->push_back (move (p));
      }

      // If there aren't any, then we can reuse already merged cmd_vars (they
      // don't change during evaluate_*() calls except for the dependency
      // overrides case).
      //
      const strings& cmd_vars (
        merge_cmd_vars (dependent_vars_,
                        dependency_vars,
                        dependency_vars.empty () /* cache */));

      auto rsi (bootstrap (*this, cmd_vars));
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
      // This is also where we set dependency configuration variables with the
      // default and buildfile origins and typify all dependency variables
      // (see evaluate_prefer_accept() for details).
      //
      function<void (parser&)> pre;

      struct data
      {
        scope& rs;
        const dependency_configurations& cfgs;
        bool defaults;
      } d {rs, cfgs, defaults};

      if (!reflect_frag_.empty ()       ||
          !dependency_reflect_.empty () ||
          !cfgs.empty ())
      {
        pre = [this, &d] (parser& p)
        {
          scope& rs (d.rs);

          auto insert_var = [&rs] (const string& name,
                                   const optional<string>& type)
            -> const variable&
          {
            const value_type* vt (nullptr);
            if (type)
            {
              vt = parser::find_value_type (&rs, *type);
              assert (vt != nullptr);
            }

            return rs.var_pool ().insert (name, vt);
          };

          if (!reflect_frag_.empty ())
          {
            istringstream is (reflect_frag_);
            is.exceptions (istringstream::failbit | istringstream::badbit);

            // Note that the fragment is just a bunch of variable assignments
            // and thus unlikely to cause any errors.
            //
            path_name in ("<accumulated-reflect-fragment>");
            p.parse_buildfile (is, in, &rs, rs);
          }

          // Note that for now we don't bother setting overridden reflect
          // values as overrides. It seems the only reason to go through the
          // trouble would be to get the accurate $origin() result. But basing
          // any decisions on whether the reflect value was overridden or not
          // seems far fetched.
          //
          for (const reflect_variable_value& v: dependency_reflect_)
          {
            const variable& var (insert_var (v.name, v.type));
            value& val (rs.assign (var));

            if (v.value)
              val.assign (names (*v.value), &var);
            else
              val = nullptr;
          }

          for (const package_configuration& cfg: d.cfgs)
          {
            for (const config_variable_value& v: cfg)
            {
              const variable& var (insert_var (v.name, v.type));

              switch (v.origin)
              {
              case variable_origin::default_:
              case variable_origin::buildfile:
                {
                  if (d.defaults)
                  {
                    auto& val (
                      static_cast<variable_map::value_data&> (
                        rs.assign (var)));

                    if (v.value)
                      val.assign (names (*v.value), &var);
                    else
                      val = nullptr;

                    val.extra = v.origin == variable_origin::default_ ? 1 : 2;
                  }
                  break;
                }
              case variable_origin::undefined:
              case variable_origin::override_: break;
              }
            }
          }
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

  // Bootstrap the package skeleton.
  //
  static build2::scope_map::iterator
  bootstrap (package_skeleton& skl, const strings& cmd_vars)
  {
    assert (skl.db_ != nullptr && skl.ctx_ == nullptr);

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
      const available_package& ap (*skl.available);

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
}
