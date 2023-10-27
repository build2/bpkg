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
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/lexer.hxx>
#include <libbuild2/parser.hxx>

#include <libbuild2/config/utility.hxx>

#include <bpkg/bpkg.hxx>
#include <bpkg/package.hxx>
#include <bpkg/database.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  // Check whether the specified configuration variable override has a project
  // variable (i.e., its name starts with config.<project>). If the last
  // argument is not NULL, then set it to the length of the variable portion.
  //
  // Note that some user-specified variables may have qualifications
  // (global, scope, etc) but there is no reason to expect any project
  // configuration variables to use such qualifications (since they can
  // only apply to one project). So we ignore all qualified variables.
  //
  static inline bool
  project_override (const string& v, const string& p, size_t* l = nullptr)
  {
    size_t n (p.size ());

    if (v.compare (0, n, p) == 0)
    {
      if (v[n] == '.')
      {
        if (l != nullptr)
          *l = v.find_first_of ("=+ \t", n + 1);

        return true;
      }
      else if (strchr ("=+ \t", v[n]) != nullptr)
      {
        if (l != nullptr)
          *l = n;

        return true;
      }
    }

    return false;
  }

  // Check whether the specified configuration variable name is a project
  // variable (i.e., its name starts with config.<project>).
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
    lookup_variable (build2::names&& qual,
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

  static void
  create_context (package_skeleton&, const strings&);

  // Note: cannot be package_skeleton member function due to iterator return
  // (build2 stuff is only forward-declared in the header).
  //
  static build2::scope_map::iterator
  bootstrap (package_skeleton&, const strings&, bool old = false);

  package_skeleton::
  ~package_skeleton ()
  {
  }

  package_skeleton::
  package_skeleton (package_skeleton&& v) noexcept
      : package (move (v.package)),
        system (v.system),
        available (move (v.available)),
        load_config_flags (v.load_config_flags),
        co_ (v.co_),
        db_ (v.db_),
        var_prefix_ (move (v.var_prefix_)),
        config_vars_ (move (v.config_vars_)),
        config_var_srcs_ (move (v.config_var_srcs_)),
        disfigure_ (v.disfigure_),
        config_srcs_ (v.config_srcs_),
        src_root_ (move (v.src_root_)),
        out_root_ (move (v.out_root_)),
        src_root_specified_ (v.src_root_specified_),
        old_src_root_ (move (v.old_src_root_)),
        old_out_root_ (move (v.old_out_root_)),
        created_ (v.created_),
        verified_ (v.verified_),
        loaded_old_config_ (v.loaded_old_config_),
        develop_ (v.develop_),
        ctx_ (move (v.ctx_)),
        rs_ (v.rs_),
        cmd_vars_ (move (v.cmd_vars_)),
        cmd_vars_cache_ (v.cmd_vars_cache_),
        dependent_vars_ (move (v.dependent_vars_)),
        dependent_orgs_ (move (v.dependent_orgs_)),
        reflect_ (move (v.reflect_)),
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
  operator= (package_skeleton&& v) noexcept
  {
    if (this != &v)
    {
      package = move (v.package);
      system = v.system;
      available = move (v.available);
      load_config_flags = v.load_config_flags;
      co_ = v.co_;
      db_ = v.db_;
      var_prefix_ = move (v.var_prefix_);
      config_vars_ = move (v.config_vars_);
      config_var_srcs_ = move (v.config_var_srcs_);
      disfigure_ = v.disfigure_;
      config_srcs_ = v.config_srcs_;
      src_root_ = move (v.src_root_);
      out_root_ = move (v.out_root_);
      src_root_specified_ = v.src_root_specified_;
      old_src_root_ = move (v.old_src_root_);
      old_out_root_ = move (v.old_out_root_);
      created_ = v.created_;
      verified_ = v.verified_;
      loaded_old_config_ = v.loaded_old_config_;
      develop_ = v.develop_;
      ctx_ = move (v.ctx_);
      rs_ = v.rs_;
      cmd_vars_ = move (v.cmd_vars_);
      cmd_vars_cache_ = v.cmd_vars_cache_;
      dependent_vars_ = move (v.dependent_vars_);
      dependent_orgs_ = move (v.dependent_orgs_);
      reflect_ = move (v.reflect_);
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
      : package (v.package),
        system (v.system),
        available (v.available),
        load_config_flags (v.load_config_flags),
        co_ (v.co_),
        db_ (v.db_),
        var_prefix_ (v.var_prefix_),
        config_vars_ (v.config_vars_),
        config_var_srcs_ (v.config_var_srcs_),
        disfigure_ (v.disfigure_),
        config_srcs_ (v.config_srcs_),
        src_root_ (v.src_root_),
        out_root_ (v.out_root_),
        src_root_specified_ (v.src_root_specified_),
        old_src_root_ (v.old_src_root_),
        old_out_root_ (v.old_out_root_),
        created_ (v.created_),
        verified_ (v.verified_),
        loaded_old_config_ (v.loaded_old_config_),
        develop_ (v.develop_),
        cmd_vars_ (v.cmd_vars_),
        cmd_vars_cache_ (v.cmd_vars_cache_),
        dependent_vars_ (v.dependent_vars_),
        dependent_orgs_ (v.dependent_orgs_),
        reflect_ (v.reflect_),
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

    reflect_.clear ();

    dependency_reflect_.clear ();
    dependency_reflect_index_ = 0;
    dependency_reflect_pending_ = 0;

    dependency_var_prefixes_.clear ();
    dependency_var_prefixes_pending_ = 0;

    prefer_accept_ = nullopt;
  }

  package_skeleton::
  package_skeleton (const common_options& co,
                    package_key pk,
                    bool sys,
                    shared_ptr<const available_package> ap,
                    strings cvs,
                    bool df,
                    const vector<config_variable>* css,
                    optional<dir_path> src_root,
                    optional<dir_path> out_root,
                    optional<dir_path> old_src_root,
                    optional<dir_path> old_out_root,
                    uint16_t lcf)
      : package (move (pk)),
        system (sys),
        available (move (ap)),
        load_config_flags (lcf),
        co_ (&co),
        db_ (&package.db.get ()),
        var_prefix_ ("config." + package.name.variable ()),
        config_vars_ (move (cvs)),
        disfigure_ (df),
        config_srcs_ (df ? nullptr : css)
  {
    if (available != nullptr)
      assert (available->bootstrap_build); // Should have skeleton info.
    else
      assert (system);

    if (!config_vars_.empty ())
      config_var_srcs_ = vector<config_source> (config_vars_.size (),
                                                config_source::user);

    // We are only interested in old user configuration variables.
    //
    if (config_srcs_ != nullptr)
    {
      if (find_if (config_srcs_->begin (), config_srcs_->end (),
                   [this] (const config_variable& v)
                   {
                     return ((load_config_flags & load_config_user) != 0 &&
                             v.source == config_source::user) ||
                            ((load_config_flags & load_config_dependent) != 0 &&
                             v.source == config_source::dependent);
                   }) == config_srcs_->end ())
        config_srcs_ = nullptr;
    }

    // We don't need to load old user configuration if there isn't any and
    // there is no new project configuration specified by the user.
    //
    // Note that at first it may seem like we shouldn't do this for any system
    // packages but if we want to verify the user configuration, why not do so
    // for system if we can (i.e., have skeleton info)?
    //
    if (available == nullptr)
      loaded_old_config_ = true;
    else
      loaded_old_config_ =
        (config_srcs_ == nullptr) &&
        find_if (config_vars_.begin (), config_vars_.end (),
                 [this] (const string& v)
                 {
                   // For now tighten it even further so that we can continue
                   // using repositories without package skeleton information
                   // (bootstrap.build, root.build). See
                   // load_old_config_impl() for details.
                   //
#if 0
                   return project_override (v, var_prefix_);
#else
                   size_t vn;
                   size_t pn (var_prefix_.size ());
                   return (project_override (v, var_prefix_, &vn) &&
                           v.compare (pn, vn - pn, ".develop") == 0);
#endif
                 }) == config_vars_.end ();

    if (src_root)
    {
      src_root_ = move (*src_root);

      assert (!src_root_.empty ()); // Must exist.

      src_root_specified_ = true;

      if (out_root)
        out_root_ = move (*out_root);
    }
    else
      assert (!out_root);

    if (old_src_root)
    {
      old_src_root_ = move (*old_src_root);

      assert (!old_src_root_.empty ()); // Must exist.

      if (old_out_root)
        old_out_root_ = move (*old_out_root);
    }
    else
      assert (!old_out_root);
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
      names_view nv (reverse (val, storage, true /* reduce */));

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

  // Reverse value to names reducing empty simple value to empty list of
  // names.
  //
  static optional<build2::names>
  reverse_value (const build2::value& val)
  {
    using namespace build2;

    if (val.null)
      return nullopt;

    names storage;
    names_view nv (reverse (val, storage, true /* reduce */));

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
            reflect_.empty ()            &&
            dependency_reflect_.empty () &&
            available != nullptr         &&
            ctx_ == nullptr);

    if (!loaded_old_config_)
      load_old_config_impl ();

    try
    {
      using namespace build2;
      using build2::info;

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
      scope* rs;
      {
        auto df = build2::make_diag_frame (
          [this] (const build2::diag_record& dr)
          {
            dr << info << "while loading build system skeleton of package "
               << package.name;
          });

        rs = bootstrap (
          *this, merge_cmd_vars (dependent_cmd_vars (cfg)))->second.front ();

        // Load project's root.build.
        //
        load_root (*rs);
      }

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
      // Note: go straight for the public variable pool.
      //
      for (const variable& var: rs->ctx.var_pool)
      {
        if (!project_variable (var.name, var_prefix_))
          continue;

        using config::variable_origin;

        pair<variable_origin, lookup> ol (config::origin (*rs, var));

        switch (ol.first)
        {
        case variable_origin::default_:
        case variable_origin::override_:
        case variable_origin::undefined:
          {
            config_variable_value v {
              var.name, ol.first, {}, {}, {}, false, false};

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
                  v.has_alternative = ov->has_alternative;
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

  void package_skeleton::
  load_overrides (package_configuration& cfg)
  {
    // Should only be called before dependent_config()/evaluate_*() and only
    // on system package without skeleton info.
    //
    assert (dependent_vars_.empty ()     &&
            reflect_.empty ()            &&
            dependency_reflect_.empty () &&
            available == nullptr         &&
            system);

    if (find_if (config_vars_.begin (), config_vars_.end (),
                 [this] (const string& v)
                 {
                   return project_override (v, var_prefix_);
                 }) == config_vars_.end ())
      return;

    try
    {
      using namespace build2;
      using build2::fail;
      using build2::endf;

      using config::variable_origin;

      // Create the build context.
      //
      create_context (*this, strings {});
      context& ctx (*ctx_);

      // Note: go straight for the public variable pool.
      //
      scope& gs (ctx.global_scope.rw ());
      auto& vp (gs.var_pool (true /* public */));

      for (const string& v: config_vars_)
      {
        size_t vn;
        if (!project_override (v, var_prefix_, &vn))
          continue;

        const variable& var (vp.insert (string (v, 0, vn)));

        // Parse the value part (note that all evaluate_require() cares about
        // is whether the value is true or not, but we do need accurate values
        // for diagnostics).
        //
        size_t p (v.find ('=', vn));
        assert (p != string::npos);
        if (v[p + 1] == '+')
          ++p;

        optional<names> val;
        {
          // Similar to context() ctor.
          //
          istringstream is (string (v, p + 1));
          is.exceptions (istringstream::failbit | istringstream::badbit);

          path_name in ("<cmdline>");
          lexer lex (is, in, 1 /* line */, "\'\"\\$("); // Effective.

          parser par (ctx);
          pair<value, token> r (
            par.parse_variable_value (lex, gs, &build2::work, var));

          if (r.second.type != token_type::eos)
            fail << "invalid command line variable override '" << v << "'";

          val = reverse_value (r.first);
        }

        // @@ Should we do anything with append/prepend?
        //
        if (config_variable_value* v = cfg.find (var.name))
          v->value = move (val);
        else
          cfg.push_back (config_variable_value {var.name,
                                                variable_origin::override_,
                                                {},
                                                move (val),
                                                {},
                                                false,
                                                false});
      }

      ctx_ = nullptr; // Free.
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
            reflect_.empty ()            &&
            dependency_reflect_.empty () &&
            available != nullptr         &&
            ctx_ == nullptr);

    if (!loaded_old_config_)
      load_old_config_impl ();

    try
    {
      using namespace build2;
      using build2::info;

      // For now we treat any failure to load root.build as bad configuration,
      // which is not very precise. One idea to make this more precise would
      // be to invent some kind of tagging for "bad configuration" diagnostics
      // (e.g., either via an attribute or via special config.assert directive
      // or some such).
      //
      // For now we rely on load_defaults() and load_old_config_impl() to
      // "flush" out any unrelated errors (e.g., one of the modules
      // configuration is bad, etc). However, if that did not happen
      // naturally, then we must do it ourselves.
      //
      if (!verified_)
      {
        auto df = build2::make_diag_frame (
          [this] (const build2::diag_record& dr)
          {
            dr << info << "while loading build system skeleton of package "
               << package.name;
          });

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
      // Note: no diag_frame unlike all the other places.
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
  // Note that currently we only use this function for the being reconfigured
  // and external packages (i.e. when the existing source directory is
  // specified). We could also do something similar for the remaining cases by
  // pointing to the manifest we have serialized. In this case we would also
  // need to make sure the temp directory is not cleaned in case of an error.
  // Maybe one day.
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

      // Evaluate the enable condition.
      //
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
        [this, &cond, depends_index] (const build2::diag_record& dr)
        {
          dr << info << "enable condition: (" << cond << ")";

          // If an existing source directory has been specified, then we have
          // the manifest and so print the location of the depends value in
          // questions.
          //
          if (src_root_specified_)
            depends_location (dr, src_root_ / manifest_file, depends_index);
          else
            dr << info << "in depends manifest value of package "
               << package.name;
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
    // They could also clash with dependent configuration. Probably should be
    // handled in the same way (it's just another type of "user"). Yes, since
    // dependent_vars_ are entered as cmd line overrides, this is how they are
    // treated.
    //
    // It seems like the most straightforward way to achieve the desired
    // semantics with the mechanisms that we have (in other words, without
    // inventing another "level" of overrides) is to evaluate the reflect
    // fragment after loading root.build. This way it will (1) be able to use
    // variables set by root.build in conditions, (2) override default values
    // of configuration variables (and those loaded from config.build), and
    // (3) be overriden by configuration variables specified by the user.
    // Naturally, this approach is probably not without a few corner cases.
    //
    // We may also have configuration values from the previous reflect clause
    // which we want to "factor in" before evaluating the next clause (enable,
    // reflect etc.; so that they can use the previously reflected values or
    // values that are derived from them in root.build). It seems like we have
    // two options here: either enter them as true overrides similar to
    // config_vars_ or just evaluate them similar to loading config.build
    // (which, BTW, we might have, in case of a being reconfigured or external
    // package). The big problem with the former approach is that it will then
    // prevent any further reflect clauses from modifying the same values.
    //
    // So overall it feels like we have iterative/compartmentalized
    // configuration process. A feedback loop, in a sense. And it's the
    // responsibility of the package author (who is in control of both
    // root.build and manifest) to arrange for suitable compartmentalization.
    //
    try
    {
      // Note: similar in many respects to evaluate_enable().
      //
      using namespace build2;
      using config::variable_origin;
      using build2::diag_record;
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

      // Collect all the set config.<name>.* variables on the first pass and
      // filter out unchanged on the second.
      //
      // Note: a lot of this code is inspired by the config module.
      //
      struct rvar
      {
        const variable* var;
        const value*    val;
        size_t          ver;
      };

      class rvars: public vector<rvar>
      {
      public:
        pair<iterator, bool>
        insert (const rvar& v)
        {
          auto i (find_if (begin (), end (),
                           [&v] (const rvar& o) {return v.var == o.var;}));
          if (i != end ())
            return make_pair (i, false);

          push_back (v);
          return make_pair (--end (), true);
        }
      };

      rvars vars;

      auto process = [this, &rs, &vars] (bool collect)
      {
        // @@ TODO: would be nice to verify no bogus variables are set (can
        //    probably only be done via the saved variables map).

        for (auto p (rs.vars.lookup_namespace (var_prefix_));
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
            vars.insert (rvar {&var, nullptr, val.version});
          else
          {
            auto p (vars.insert (rvar {&var, &val, 0}));

            if (!p.second)
            {
              auto i (p.first);

              if (i->ver == val.version)
                vars.erase (i); // Unchanged.
              else
                i->val = &val;
            }
          }
        }
      };

      // Evaluate the reflect clause.
      //
      istringstream is (refl);
      is.exceptions (istringstream::failbit | istringstream::badbit);

      path_name in ("<depends-reflect-clause>");
      uint64_t il (1);

      // Note: keep it active until the end (see the override detection).
      //
      auto df = build2::make_diag_frame (
        [this, &refl, depends_index] (const build2::diag_record& dr)
        {
          // Probably safe to assume a one-line fragment contains a variable
          // assignment.
          //
          if (refl.find ('\n') == string::npos)
            dr << info << "reflect variable: " << trim (string (refl));
          else
            dr << info << "reflect clause:\n"
               << trim_right (string (refl));

          // If an existing source directory has been specified, then we have
          // the manifest and so print the location of the depends value in
          // questions.
          //
          if (src_root_specified_)
            depends_location (dr, src_root_ / manifest_file, depends_index);
          else
            dr << info << "in depends manifest value of package "
               << package.name;
        });

      lexer l (is, in, il /* start line */);
      buildfile_parser p (rs.ctx,
                          dependency_var_prefixes_,
                          dependency_var_prefixes_pending);

      process (true);
      p.parse_buildfile (l, &rs, rs);
      process (false);

      // Add to the vars set the reflect variables collected previously.
      //
      for (const reflect_variable_value& v: reflect_)
      {
        // Note: go straight for the public variable pool.
        //
        const variable* var (rs.ctx.var_pool.find (v.name));
        assert (var != nullptr); // Must be there (set by load()).

        auto p (vars.insert (rvar {var, nullptr, 0}));

        if (p.second)
          p.first->val = rs.vars[*var].value; // Must be there (set by load()).
      }

      // Re-populate everything from the var set but try to re-use buffers as
      // much a possible (normally we would just be appending more variables
      // at the end).
      //
      reflect_.resize (vars.size ());

      // Collect the config.<name>.* variables that were changed by this
      // and previous reflect clauses.
      //
      for (size_t i (0); i != vars.size (); ++i)
      {
        const variable& var (*vars[i].var);
        const value& val (*vars[i].val);

        pair<variable_origin, lookup> ol (
          config::origin (rs,
                          var,
                          pair<lookup, size_t> {
                            lookup {val, var, rs.vars}, 1 /* depth */}));

        reflect_variable_value& v (reflect_[i]);
        v.name = var.name;
        v.origin = ol.first;

        if (ol.first == variable_origin::override_)
        {
          // Detect an overridden reflect value, but allowing it if the values
          // match (think both user/dependent and reflect trying to enable the
          // same feature).
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
          // Note also that a sufficiently smart reflect clause can detect if
          // a variable is overridden (with $config.origin()) and avoid the
          // clash. Perhaps that should be the recommendation for reflect
          // variables that can also plausibly be set by the user (it feels
          // like configuration variables have the intf/impl split similar
          // to libraries)?
          //
          if (val != *ol.second)
          {
            // See if this is a dependent or user override.
            //
            const package_key* d (nullptr);
            {
              for (size_t i (0); i != dependent_vars_.size (); ++i)
              {
                const string& v (dependent_vars_[i]);
                size_t n (var.name.size ());
                if (v.compare (0, n, var.name) == 0 && v[n] == '=')
                {
                  d = &dependent_orgs_[i];
                  break;
                }
              }
            }

            diag_record dr (fail);

            dr << "reflect variable " << var << " overriden by ";

            if (d != nullptr)
              dr << "dependent " << *d;
            else
              dr << "user configuration";

            names storage;
            dr << info << "reflect value: "
               << serialize_cmdline (var.name, val, storage);

            dr << info << (d != nullptr ? "dependent" : "user") << " value: "
               << serialize_cmdline (var.name, *ol.second, storage);
          }

          // Skipped in load(), collect_config(), but used in print_config().
        }
        else
        {
          assert (ol.first == variable_origin::buildfile);

          // Note that we keep it always untyped letting the config directives
          // in root.build to take care of typing.
          //
          v.value = reverse_value (val);
        }
      }

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
                          pair<size_t, size_t> indexes,
                          bool has_alt)
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
          [this, &prefer, depends_index] (const build2::diag_record& dr)
          {
            dr << info << "prefer clause:\n"
               << trim_right (string (prefer));

            // If an existing source directory has been specified, then we
            // have the manifest and so print the location of the depends
            // value in questions.
            //
            if (src_root_specified_)
              depends_location (dr, src_root_ / manifest_file, depends_index);
            else
              dr << info << "in depends manifest value of package "
                 << package.name;
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
                info << var.name << " set in prefer clause of dependent "
                   << package.string ();
            }
          }
        }
      }

      // Evaluate the accept condition.
      //
      bool r;
      {
        istringstream is ('(' + accept + ')');
        is.exceptions (istringstream::failbit | istringstream::badbit);

        path_name in ("<depends-accept-clause>");
        uint64_t il (1);

        auto df = build2::make_diag_frame (
          [this, &accept, depends_index] (const build2::diag_record& dr)
          {
            dr << info << "accept condition: (" << accept << ")";

            // If an existing source directory has been specified, then we
            // have the manifest and so print the location of the depends
            // value in questions.
            //
            if (src_root_specified_)
              depends_location (dr, src_root_ / manifest_file, depends_index);
            else
              dr << info << "in depends manifest value of package "
                 << package.name;
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
                   << "dependent " << package.string () << " but not dependency" <<
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
                v.dependent = package; // We are the originating dependent.
                v.confirmed = true;
                v.has_alternative = has_alt;
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
                    const string& require,
                    pair<size_t, size_t> indexes,
                    bool has_alt)
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
      // information as well as overrides. Except that the type information
      // may not be available for system packages, so we must deal with
      // that. See negotiate_configuration() for details.
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
          [this, &require, depends_index] (const build2::diag_record& dr)
          {
            dr << info << "require clause:\n"
               << trim_right (string (require));

            // If an existing source directory has been specified, then we
            // have the manifest and so print the location of the depends
            // value in questions.
            //
            if (src_root_specified_)
              depends_location (dr, src_root_ / manifest_file, depends_index);
            else
              dr << info << "in depends manifest value of package "
                 << package.name;
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

            const value& val (p.first->second);

            // Deal with a system package that has no type information.
            //
            if (!cfg.system)
            {
              const config_variable_value* v (cfg.find (var.name));

              if (v == nullptr)
              {
                fail << "package " << cfg.package.name << " has no "
                     << "configuration variable " << var.name <<
                  info << var.name << " set in require clause of dependent "
                     << package.string ();
              }

              if (!v->type || *v->type != "bool")
              {
                fail << "configuration variable " << var.name << " is not of "
                     << "bool type" <<
                  info << var.name << " set in require clause of dependent "
                     << package.string ();
              }
            }

            bool r;
            if (cfg.system)
            {
              try
              {
                r = build2::convert<bool> (val);
              }
              catch (const invalid_argument&)
              {
                r = false;
              }
            }
            else
              r = cast_false<bool> (val);

            if (!r)
            {
              fail << "configuration variable " << var.name << " is not set "
                   << "to true" <<
                info << var.name << " set in require clause of dependent "
                   << package.string ();
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

          // Note: could be NULL if cfg.system.
          //
          const config_variable_value* v (cfg.find (var.name));

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
          if (v != nullptr && v->origin == variable_origin::override_)
          {
            assert (ol.first == variable_origin::override_);
          }
          else if (ol.first == variable_origin::override_ &&
                   (v == nullptr || v->origin != variable_origin::override_))
          {
            fail << "dependency override " << var.name << " specified for "
                 << "dependent " << package.string () << " but not dependency" <<
              info << "did you mean to specify ?" << cfg.package.name
                 << " +{ " << var.name << "=... }";
          }

          if (ol.first == variable_origin::override_)
          {
            if (cfg.system)
            {
              try
              {
                if (!build2::convert<bool> (*ol.second))
                  r = false;
              }
              catch (const invalid_argument&)
              {
                r = false;
              }
            }
            else
            {
              if (!cast_false<bool> (*ol.second))
                r = false;
            }
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

            config_variable_value* v (cfg.find (var.name));

            if (v == nullptr) // cfg.system
            {
              cfg.push_back (config_variable_value {var.name,
                                                    variable_origin::undefined,
                                                    {},
                                                    {},
                                                    {},
                                                    false,
                                                    false});

              v = &cfg.back ();
            }

            // This value was set so save it as a dependency reflect.
            //
            // Note that unlike the equivalent evaluate_prefer_accept() logic,
            // here the value cannot be the default/buildfile (since we don't
            // set those; see the load() call above).
            //
            optional<names> ns (names {name ("true")});

            // Note: force bool type if system.
            //
            dependency_reflect_.push_back (
              reflect_variable_value {
                v->name,
                (v->origin == variable_origin::override_
                 ? v->origin
                 : variable_origin::buildfile),
                cfg.system ? optional<string> ("bool") : v->type,
                ns});

            if (v->origin != variable_origin::override_)
            {
              // Possible transitions:
              //
              // default/undefine -> buildfile -- override dependency default
              // buildfile        -> buildfile -- override other dependent
              //

              if (v->origin == variable_origin::buildfile)
              {
                // If unchanged, then we keep the old originating dependent
                // (even if the value was technically "overwritten" by this
                // dependent).
                //
                if (v->value == ns)
                  continue;
              }
              else
                v->origin = variable_origin::buildfile;

              v->value = move (ns);
              v->dependent = package; // We are the originating dependent.
              v->confirmed = true;
              v->has_alternative = has_alt;
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
  empty_print ()
  {
    if (!loaded_old_config_)
      load_old_config_impl ();

    return (dependent_vars_.empty () &&
            reflect_.empty ()        &&
            find_if (config_vars_.begin (), config_vars_.end (),
                     [this] (const string& v)
                     {
                       // See print_config() for details.
                       //
                       size_t vn;
                       if (project_override (v, var_prefix_, &vn))
                       {
                         if (!develop_)
                         {
                           size_t pn (var_prefix_.size ());
                           if (v.compare (pn, vn - pn, ".develop") == 0)
                             return false;
                         }

                         return true;
                       }
                       return false;
                     }) == config_vars_.end ());
  }

  void package_skeleton::
  print_config (ostream& os, const char* indent)
  {
    using build2::config::variable_origin;

    if (!loaded_old_config_)
      load_old_config_impl ();

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

    // NOTE: see also empty_print() if changing anything here.

    // @@ TODO: we could have added the package itself to "set by ..."
    //    for overriden (to the same value) reflect. But then why not
    //    do the same for dependent that is overriden by user (we could
    //    have kept them as reflect_variable_values rather than strings)?
    //    Maybe one day.

    // First comes the user configuration.
    //
    for (size_t i (0); i != config_vars_.size (); ++i)
    {
      const string& v (config_vars_[i]);

      size_t vn;
      if (project_override (v, var_prefix_, &vn))
      {
        // To reduce the noise (e.g., during bdep-init), skip
        // config.<project>.develop if the package doesn't use it.
        //
        if (!develop_)
        {
          size_t pn (var_prefix_.size ());
          if (v.compare (pn, vn - pn, ".develop") == 0)
            continue;
        }

        const char* s (nullptr);

        switch (config_var_srcs_[i])
        {
        case config_source::user: s = "user"; break;
        case config_source::dependent: s = "dependent"; break;
        case config_source::reflect: assert (false); // Must never be loaded.
        }

        print (v) << " (" << (system ? "expected " : "")
                  << s << " configuration)";
      }
    }

    // Next dependent configuration.
    //
    for (size_t i (0); i != dependent_vars_.size (); ++i)
    {
      const string& v (dependent_vars_[i]);
      const package_key& d (dependent_orgs_[i]); // Parallel.

      print (v) << " (" << (system ? "expected" : "set") << " by "
                << d << ')';
    }

    // Finally reflect (but skip overriden).
    //
    for (const reflect_variable_value& v: reflect_)
    {
      if (v.origin == variable_origin::override_)
        continue;

      string s (serialize_cmdline (v.name, v.value));
      print (s) << " (" << (system ? "expected" : "set") << " by "
                << package.name << ')';
    }
  }

  void package_skeleton::
  load_old_config ()
  {
    if (!loaded_old_config_)
      load_old_config_impl ();
  }

  pair<strings, vector<config_variable>> package_skeleton::
  collect_config () &&
  {
    // NOTE: remember to update config_checksum() if changing anything here.

    assert (db_ != nullptr); // Must be called only once.

    using build2::config::variable_origin;

    if (!loaded_old_config_)
      load_old_config_impl ();

    // Merge all the variables into a single list in the correct order
    // and assign their sources while at it.
    //
    strings vars;
    vector<config_variable> srcs;

    if (size_t n = (config_vars_.size () +
                    dependent_vars_.size () +
                    reflect_.size ()))
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
        size_t pn (var_prefix_.size ());
        for (const string& v: config_vars_)
        {
          size_t vn;
          if (project_override (v, var_prefix_, &vn))
          {
            // Skip config.<project>.develop (can potentially be passed by
            // bdep-init) if the package doesn't use it.
            //
            if (!develop_ && v.compare (pn, vn - pn, ".develop") == 0)
              continue;

            string n (v, 0, vn);

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
      if (!reflect_.empty ())
      {
        vars.reserve (n);

        // These are all project variables. There should also be no duplicates
        // by construction (see evaluate_reflect()).
        //
        for (const reflect_variable_value& v: reflect_)
        {
          if (v.origin == variable_origin::override_)
            continue;

          vars.push_back (serialize_cmdline (v.name, v.value));
          srcs.push_back (config_variable {v.name, config_source::reflect});
        }
      }
    }

    ctx_ = nullptr; // Free.
    db_ = nullptr;

    return make_pair (move (vars), move (srcs));
  }

  string package_skeleton::
  config_checksum ()
  {
    // Note: this is parallel to collect_config() logic but is not destructive.

    assert (db_ != nullptr); // Must be called before collect_config().

    if (!loaded_old_config_)
      load_old_config_impl ();

    sha256 cs;

    if (!config_vars_.empty ())
    {
      cstrings vs;
      size_t pn (var_prefix_.size ());
      for (const string& v: config_vars_)
      {
        size_t vn;
        if (project_override (v, var_prefix_, &vn))
        {
          // Skip config.<project>.develop (can potentially be passed by
          // bdep-init) if the package doesn't use it.
          //
          if (develop_ || v.compare (pn, vn - pn, ".develop") != 0)
            cs.append (v);
        }
      }
    }

    if (!dependent_vars_.empty ())
    {
      for (const string& v: dependent_vars_)
        cs.append (v);
    }

    if (!reflect_.empty ())
    {
      for (const reflect_variable_value& v: reflect_)
      {
        if (v.origin != build2::config::variable_origin::override_)
          cs.append (serialize_cmdline (v.name, v.value));
      }
    }

    return !cs.empty () ? cs.string () : string ();
  }

  const strings& package_skeleton::
  merge_cmd_vars (const strings& dependent_vars,
                  const strings& dependency_vars,
                  bool cache)
  {
    // Merge variable overrides (note that the order is important). See also a
    // custom/optimized version in load_old_config_impl().
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
          v += package.name.variable ();
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
  load_old_config_impl ()
  {
    assert (!loaded_old_config_ && ctx_ == nullptr);

    try
    {
      using namespace build2;
      using build2::info;

      // This load that must be done without config.config.disfigure. Also, it
      // would be nice to optimize for the common case where the only load is
      // to get the old configuration (e.g., config.*.develop) as part of
      // collect_config(). So instead of calling merge_cmd_vars() we will do
      // our own (but consistent) thing.
      //
      const strings* cmd_vars (nullptr);
      {
        assert (!cmd_vars_cache_); // Sanity check (we are always first).

        const strings& vs1 (build2_cmd_vars);
        const strings& vs2 (config_vars_);

        if (!disfigure_)
          cmd_vars = (vs2.empty () ? &vs1 : vs1.empty () ? &vs2 : nullptr);

        if (cmd_vars == nullptr)
        {
          // Note: the order is important (see merge_cmd_vars()).
          //
          cmd_vars_.reserve ((disfigure_ ? 1 : 0) + vs1.size () + vs2.size ());

          // If the package is being disfigured, then don't load config.build
          // at all.
          //
          if (disfigure_)
            cmd_vars_.push_back ("config.config.unload=true");

          cmd_vars_.insert (cmd_vars_.end (), vs1.begin (), vs1.end ());
          cmd_vars_.insert (cmd_vars_.end (), vs2.begin (), vs2.end ());

          cmd_vars = &cmd_vars_;
        }
      }

      scope* rs;
      {
        auto df = build2::make_diag_frame (
          [this] (const build2::diag_record& dr)
          {
            dr << info << "while loading build system skeleton of package "
               << package.name;
          });

        rs = bootstrap (*this, *cmd_vars, true /* old */)->second.front ();

        // Load project's root.build.
        //
        load_root (*rs);
      }

      // Note: go straight for the public variable pool.
      //
      if (const variable* var = rs->ctx.var_pool.find (var_prefix_ + ".develop"))
      {
        // Use the fact that the variable is typed as a proxy for it being
        // defined with config directive (the more accurate way would be via
        // the config module's saved variables map).
        //
        develop_ = (var->type != nullptr);
      }

      // @@ TODO: should we also verify user-specified project configuration
      //    variables are not bogus? But they could be untyped...
      //
      //    Also, build2 warns about unused variables being dropped.
      //
      //    Note that currently load_old_config_impl() is disabled unless
      //    there is a config.*.develop variable or we were asked to load
      //    dependent configuration; see package_skeleton ctor.

      // Extract and merge old user and/or dependent configuration variables
      // from config.build (or equivalent) into config_vars.
      //
      if (config_srcs_ != nullptr)
      {
        assert (!disfigure_);

        auto i (config_vars_.begin ());     // Insert position, see below.
        auto j (config_var_srcs_.begin ()); // Insert position, see below.

        names storage;
        for (const config_variable& v: *config_srcs_)
        {
          if (!(((load_config_flags & load_config_user) != 0 &&
                 v.source == config_source::user) ||
                ((load_config_flags & load_config_dependent) != 0 &&
                 v.source == config_source::dependent)))
            continue;

          using config::variable_origin;

          pair<variable_origin, lookup> ol (config::origin (*rs, v.name));

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

              j = config_var_srcs_.insert (j, v.source) + 1;
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
      }

      loaded_old_config_ = true;

      if (old_src_root_.empty ())
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

    if (!loaded_old_config_)
      load_old_config_impl ();

    try
    {
      using namespace build2;
      using build2::info;
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

      auto df = build2::make_diag_frame (
        [this] (const build2::diag_record& dr)
        {
          dr << info << "while loading build system skeleton of package "
             << package.name;
        });

      auto rsi (bootstrap (*this, cmd_vars));
      scope& rs (*rsi->second.front ());

      // Load project's root.build as well as potentially accumulated reflect
      // variables.
      //
      // If we have the accumulated reflect variables, wedge them just before
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

      if (!cfgs.empty ()     ||
          !reflect_.empty () ||
          !dependency_reflect_.empty ())
      {
        pre = [this, &d] (parser&)
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

            // Note: go straight for the public variable pool.
            //
            return rs.var_pool (true /* public */).insert (name, vt);
          };

          for (const reflect_variable_value& v: reflect_)
          {
            if (v.origin == variable_origin::override_)
              continue;

            const variable& var (insert_var (v.name, v.type)); // Note: untyped.
            value& val (rs.assign (var));

            if (v.value)
              val.assign (names (*v.value), &var);
            else
              val = nullptr;
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

  // Create the build context.
  //
  static void
  create_context (package_skeleton& skl, const strings& cmd_vars)
  {
    assert (skl.db_ != nullptr && skl.ctx_ == nullptr);

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
                     nullopt /* match_only */,          // Shouldn't matter.
                     false   /* no_external_modules */,
                     false   /* dry_run */,             // Shouldn't matter.
                     false   /* no_diag_buffer */,      // Shouldn't matter.
                     false   /* keep_going */,          // Shouldnt' matter.
                     cmd_vars));
    }
    catch (const build2::failed&)
    {
      throw failed (); // Assume the diagnostics has already been issued.
    }
  }

  // Bootstrap the package skeleton.
  //
  static build2::scope_map::iterator
  bootstrap (package_skeleton& skl, const strings& cmd_vars, bool old)
  {
    assert (skl.db_ != nullptr       &&
            skl.ctx_ == nullptr      &&
            skl.available != nullptr);

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
    if (old && skl.old_src_root_.empty ())
      old = false;

    dir_path& skl_src_root (old ? skl.old_src_root_ : skl.src_root_);
    dir_path& skl_out_root (old ? skl.old_out_root_ : skl.out_root_);

    if (!skl.created_)
    {
      const available_package& ap (*skl.available);

      // Note that we create the skeleton directories in the skeletons/
      // subdirectory of the configuration temporary directory to make sure
      // they never clash with other temporary subdirectories (git
      // repositories, etc).
      //
      // Note: for old src/out, everything should already exist.
      //
      if (!old && (skl_src_root.empty () || skl_out_root.empty ()))
      {
        // Cannot be specified if src_root_ is unspecified.
        //
        assert (skl_out_root.empty ());

        // Note that only configurations which can be used as repository
        // information sources has the temporary directory facility
        // pre-initialized (see pkg-build.cxx for details). Thus, we may need
        // to initialize it ourselves.
        //
        const dir_path& c (skl.db_->config_orig);
        auto i (tmp_dirs.find (c));

        if (i == tmp_dirs.end ())
        {
          init_tmp (c);

          i = tmp_dirs.find (c);
        }

        // Make sure the source and out root directories, if set, are absolute
        // and normalized.
        //
        // Note: can never fail since the temporary directory should already
        // be created and so its path should be valid.
        //
        dir_path d (normalize (i->second, "temporary directory"));

        d /= "skeletons";
        d /= skl.package.name.string () + '-' + ap.version.string ();

        if (skl_src_root.empty ())
          skl_src_root = move (d); // out_root_ is the same.
        else
          skl_out_root = move (d); // Don't even need to create it.
      }

      if (!exists (skl_src_root))
      {
        assert (!old); // An old package version cannot not exist.

        // Create the buildfiles.
        //
        // Note that it's probably doesn't matter which naming scheme to use
        // for the buildfiles, unless in the future we allow specifying
        // additional files.
        //
        {
          bool an (*ap.alt_naming);

          path bf (skl_src_root /
                   (an ? alt_bootstrap_file : std_bootstrap_file));

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
            save (*ap.root_build,
                  skl_src_root / (an ? alt_root_file : std_root_file));

          for (const buildfile& f: ap.buildfiles)
          {
            path p (skl_src_root                         /
                    (an ? alt_build_dir : std_build_dir) /
                    f.path);

            p += ".";
            p += (an ? alt_build_ext : std_build_ext);

            mk_p (p.directory ());

            save (f.content, p);
          }
        }

        // Create the manifest file containing the bare minimum of values
        // which can potentially be required to load the build system state
        // (i.e., either via the version module or manual version extraction).
        //
        {
          package_manifest m;
          m.name = skl.package.name;
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

          path mf (skl_src_root / manifest_file);

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

      if (!old)
        skl.created_ = true;
    }

    try
    {
      using namespace build2;
      using build2::fail;
      using build2::endf;

      // Create the build context.
      //
      create_context (skl, cmd_vars);
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
      const dir_path& src_root (skl_src_root);
      const dir_path& out_root (skl_out_root.empty ()
                                ? skl_src_root
                                : skl_out_root);

      auto rsi (create_root (ctx, out_root, src_root));
      scope& rs (*rsi->second.front ());

      // Note: we know this project hasn't yet been bootstrapped.
      //
      optional<bool> altn;
      value& v (bootstrap_out (rs, altn));

      if (!v)
        v = src_root;
      else
      {
        // If the package directory was moved, then it's possible we will have
        // src-root.build with an old src_root value. Presumably this will
        // cause the package to be re-configured and so ignoring the old value
        // here should be ok. Note that the outdated src-root.build can also
        // mess up subproject discovery in create_bootstrap_outer() but we
        // omit that part.
        //
        if (cast<dir_path> (v) != src_root)
          v = src_root;
      }

      setup_root (rs, false /* forwarded */);

      bootstrap_pre (rs, altn);
      bootstrap_src (rs, altn,
                     skl.db_->config.relative (out_root) /* amalgamation */,
                     false                               /* subprojects */);


      // Omit discovering amalgamation's subprojects (i.e., all the packages
      // in the configuration). Besides being a performance optimization, this
      // also sidesteps the issue of outdated src-root.build (see above).
      //
      create_bootstrap_outer (rs, false /* subprojects */);
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
