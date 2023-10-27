// file      : bpkg/package-skeleton.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PACKAGE_SKELETON_HXX
#define BPKG_PACKAGE_SKELETON_HXX

#include <libbuild2/forward.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-configuration.hxx>
#include <bpkg/common-options.hxx>

namespace bpkg
{
  // A build system skeleton of a package used to evaluate buildfile clauses
  // during dependency resolution (enable, reflect, require or prefer/accept).
  //
  class package_skeleton
  {
  public:
    // If the package is system, then its available package should be NULL if
    // it doesn't match the system package version "close enough" to be usable
    // as the source of its configuration information (types, defaults). If it
    // is NULL, then the skeleton can only be used to print and collect the
    // configuration information.
    //
    // If the package is being reconfigured (rather than up/downgraded), then
    // the existing package source and output root directories (src_root and
    // out_root) need to be specified (as absolute and normalized). Otherwise,
    // if the package is external, then the existing package source root
    // directory needs to be specified (as absolute and normalized). In this
    // case, if output root is specified (as absolute and normalized; normally
    // <config-dir>/<package-name>), then it's used as is. Otherwise, an empty
    // skeleton directory is used as output root.
    //
    // If the package is neither being reconfigured nor is external, then none
    // of the root directories should be specified.
    //
    // If the package is configured as source and the user and/or dependent
    // configuration is requested to be loaded from config.build, then the
    // existing package old source and output root directories (old_src_root
    // and old_out_root) need to be specified (as absolute and normalized). If
    // specified, they are used instead of package source and output root
    // directories to load the current user and/or dependent configuration.
    // The idea here is that during package upgrade/downgrade, we want to load
    // the old configuration from the old version's src/out but then continue
    // evaluating clauses using the new version's src/out.
    //
    // The disfigure argument should indicate whether the package is being
    // reconfigured from scratch (--disfigure).
    //
    // The config_vars argument contains configuration variables specified by
    // the user in this bpkg execution. Optional config_srcs is used to
    // extract (from config.build or equivalent) configuration variables
    // specified by the user in previous bpkg executions. It should be NULL if
    // this is the first build of the package. The extracted variables are
    // merged with config_vars and the combined result is returned by
    // collect_config() below.
    //
    // @@ TODO: speaking of the "config.build or equivalent" part, the
    //    equivalent is likely to be extracted configuration (probably saved
    //    to file in tmp somewhere) that we will load with config.config.load.
    //    It doesn't seem like a good idea to pass it as part of config_vars
    //    (because sometimes we may need to omit it) so most likely it will be
    //    passed as a separate arguments (likely a file path).
    //
    // Note that the options, database, and config_srcs are expected to
    // outlive this object.
    //
    // Note also that this creates an "unloaded" skeleton and is therefore
    // relatively cheap.
    //
    package_skeleton (const common_options& co,
                      package_key,
                      bool system,
                      shared_ptr<const available_package>,
                      strings config_vars,
                      bool disfigure,
                      const vector<config_variable>* config_srcs,
                      optional<dir_path> src_root,
                      optional<dir_path> out_root,
                      optional<dir_path> old_src_root,
                      optional<dir_path> old_out_root,
                      uint16_t load_config_flags);

    package_key package;
    bool system;
    shared_ptr<const available_package> available;

    // Load package (old) configuration flags.
    //
    uint16_t load_config_flags;

    static const uint16_t load_config_user      = 0x1;
    static const uint16_t load_config_dependent = 0x2;

    // The following functions should be called in the following sequence
    // (* -- zero or more, ? -- zero or one):
    //
    // * reload_defaults() | verify_sensible()
    // ? dependent_config()
    // * evaluate_*()
    // * empty() | print_config()
    // * config_checksum()
    //   collect_config()
    //
    // Note that the load_old_config() function can be called at eny point
    // before collect_config() (and is called implicitly by most other
    // functions).
    //
    // Note that a copy of the skeleton is expected to continue with the
    // sequence rather than starting from scratch, unless reset() is called.
    //
  public:
    // Reload the default values and type information for configuration
    // variables using the values with the buildfile origin as a "tentative"
    // dependent configuration.
    //
    void
    reload_defaults (package_configuration&);

    // Load overrides for a system package without skeleton info. Note that
    // this is done in an ad hoc manner and only to support evaluate_require()
    // semantics (see the implementation for details).
    //
    void
    load_overrides (package_configuration&);

    // Verify the specified "tentative" dependent configuration is sensible,
    // that is, acceptable to the dependency itself. If it is not, then the
    // second half of the result contains the diagnostics.
    //
    pair<bool, string>
    verify_sensible (const package_configuration&);

    // Incorporate the "final" dependent configuration into subsequent
    // evaluations. Dependent configuration variables are expected not to
    // clash with user.
    //
    void
    dependent_config (const package_configuration&);

    // For the following evaluate_*() functions assume that the clause belongs
    // to the dependency alternative specified as a pair of indexes (depends
    // value index and alternative index).

    // Evaluate the enable clause.
    //
    bool
    evaluate_enable (const string&, pair<size_t, size_t>);

    // Evaluate the reflect clause.
    //
    void
    evaluate_reflect (const string&, pair<size_t, size_t>);

    // Evaluate the prefer/accept or require clauses on the specified
    // dependency configurations (serves as both input and output).
    //
    // Return true is acceptable and false otherwise. If acceptable, the
    // passed configuration is updated with new values, if any.
    //
    using dependency_configurations =
      small_vector<reference_wrapper<package_configuration>, 1>;

    bool
    evaluate_prefer_accept (const dependency_configurations&,
                            const string&, const string&, pair<size_t, size_t>,
                            bool has_alternative);

    bool
    evaluate_require (const dependency_configurations&,
                      const string&, pair<size_t, size_t>,
                      bool has_alternative);

    // Reset the skeleton to the start of the call sequence.
    //
    // Note that this function cannot be called after collect_config().
    //
    void
    reset ();

    // Return true if there are no accumulated *project* configuration
    // variables that will be printed by print_config().
    //
    bool
    empty_print ();

    // Print the accumulated *project* configuration variables as command line
    // overrides one per line with the specified indentation.
    //
    void
    print_config (ostream&, const char* indent);

    // Load the package's old configuration, unless it is already loaded.
    //
    void
    load_old_config ();

    // Return the accumulated configuration variables (first) and project
    // configuration variable sources (second). Note that the arrays are not
    // necessarily parallel (config_vars may contain non-project variables).
    //
    // Note that the dependent and reflect variables are merged with
    // config_vars/config_srcs and should be used instead rather than in
    // addition to config_vars.
    //
    // Note also that this should be the final call on this object.
    //
    pair<strings, vector<config_variable>>
    collect_config () &&;

    // Return the checksum of the project configuration variables that will be
    // returned by the collect_config() function call.
    //
    string
    config_checksum ();

    // Implementation details.
    //
  public:
    // We have to define these because context is forward-declared. Also, copy
    // constructor has some special logic.
    //
    ~package_skeleton ();
    package_skeleton (package_skeleton&&) noexcept;
    package_skeleton& operator= (package_skeleton&&) noexcept;

    package_skeleton (const package_skeleton&);
    package_skeleton& operator= (const package_skeleton&) = delete;

  private:
    // Load old user and/or dependent configuration variables from
    // config.build (or equivalent) and merge them into config_vars_ and
    // config_var_srcs_. Also verify new user configuration already in
    // config_vars_ makes sense.
    //
    // This should be done before any attempt to load the configuration with
    // config.config.disfigure and, if this did not happen, inside
    // collect_config() (since the package will be reconfigured with
    // config.config.disfigure).
    //
    void
    load_old_config_impl ();

    // (Re)load the build system state.
    //
    // Call this function before evaluating every clause.
    //
    // If dependency configurations are specified, then typify the variables
    // and set their values. If defaults is false, then only typify the
    // variables and set overrides without setting the default/buildfile
    // values. Note that buildfile values have value::extra set to 2. While
    // at it, also remove from dependency_var_prefixes_ and add to
    // dependency_var_prefixes variable prefixes (config.<project>) for
    // the passed dependencies.
    //
    build2::scope&
    load (const dependency_configurations& = {},
          strings* dependency_var_prefixes = nullptr,
          bool defaults = true);

    // Merge command line variable overrides into one list (normally to be
    // passed to bootstrap()).
    //
    // If cache is true, then assume the result can be reused on subsequent
    // calls.
    //
    const strings&
    merge_cmd_vars (const strings& dependent_vars,
                    const strings& dependency_vars = {},
                    bool cache = false);

    // Implementation details (public for bootstrap()).
    //
  public:
    // NOTE: remember to update move/copy constructors!
    //
    const common_options* co_;
    database* db_;

    string var_prefix_; // config.<project>

    strings config_vars_;

    // Configuration sources for variables in config_vars_ (parallel). Can
    // only contain config_source::{user,dependent} entries (see
    // load_old_config_impl() for details).
    //
    vector<config_source> config_var_srcs_;

    bool disfigure_;
    const vector<config_variable>* config_srcs_; // NULL if nothing to do or
                                                 // already done.

    dir_path src_root_; // Must be absolute and normalized.
    dir_path out_root_; // If empty, the same as src_root_.

    // True if the existing source root directory has been specified.
    //
    // Note that if that's the case, we can use the manifest file this
    // directory contains for diagnostics.
    //
    bool src_root_specified_ = false;

    // If specified, are used instead of {src,out}_root_ for loading of the
    // project configuration variables.
    //
    dir_path old_src_root_;
    dir_path old_out_root_;

    bool created_ = false;
    bool verified_ = false;
    bool loaded_old_config_;
    bool develop_ = true;  // Package has config.*.develop.

    unique_ptr<build2::context> ctx_;
    build2::scope* rs_ = nullptr;

    // Storage for merged build2_cmd_vars and config_vars_ and extra overrides
    // (like config.config.disfigure). If cache is true, then the existing
    // content can be reused.
    //
    strings cmd_vars_;
    bool cmd_vars_cache_ = false;

    strings             dependent_vars_; // Dependent variable overrides.
    vector<package_key> dependent_orgs_; // Dependent originators (parallel).

    // Reflect variable value storage. Used for both real reflect and
    // dependency reflect.
    //
    struct reflect_variable_value
    {
      string                          name;
      build2::config::variable_origin origin;
      optional<string>                type;
      optional<build2::names>         value;
    };

    class reflect_variable_values: public vector<reflect_variable_value>
    {
    public:
      const reflect_variable_value*
      find (const string& name)
      {
        auto i (find_if (begin (), end (),
                         [&name] (const reflect_variable_value& v)
                         {
                           return v.name == name;
                         }));
        return i != end () ? &*i : nullptr;
      }
    };

    reflect_variable_values reflect_; // Reflect variables.

    // Dependency configuration variables set by the prefer/require clauses
    // and that should be reflected in subsequent clauses.
    //
    // The same prefer/require clause could be re-evaluated multiple times in
    // which case the previous dependency reflect values from this clause (but
    // not from any previous clauses) should be dropped. This is achieved by
    // keeping track of the depends_index for the most recently evaluated
    // prefer/require clause along with the position of the first element that
    // was added by this clause. Note also that this logic does the right
    // thing if we move to a different dependency alternative withing the same
    // depends value.
    //
    reflect_variable_values dependency_reflect_;
    size_t                  dependency_reflect_index_ = 0;
    size_t                  dependency_reflect_pending_ = 0;

    // List of variable prefixes (config.<project>) of all known dependencies.
    //
    // This information is used to detect and diagnose references to undefined
    // dependency configuration variables (for example, those that were not
    // set and therefore not reflected). The pending index is used to ignore
    // the entries added by the last evaluate_prefer_accept() in the following
    // reflect clause (see prefer_accept_ below for details).
    //
    strings dependency_var_prefixes_;
    size_t  dependency_var_prefixes_pending_ = 0;

    // Position of the last successfully evaluated prefer/accept clauses.
    //
    // This information is used to make all (as opposed to only those set by
    // the prefer clause) dependency configuration variables available to the
    // reflect clause but only at the same position. This allows for some more
    // advanced configuration techniques, such as, using a feature if enabled
    // by someone else but not having any preferences ourselves.
    //
    optional<pair<size_t, size_t>> prefer_accept_;
  };
}

#endif // BPKG_PACKAGE_SKELETON_HXX
