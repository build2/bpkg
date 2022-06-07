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
    // If the package is external, then the existing package source root
    // directory needs to be specified (as absolute and normalized). In this
    // case, if output root is specified (as absolute and normalized; normally
    // <config-dir>/<package-name>), then it's used as is. Otherwise, an empty
    // skeleton directory is used as output root.
    //
    // If the package is not external, then none of the root directories
    // should be specified.
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
    // Note that the options, database, available_package, and config_srcs are
    // expected to outlive this object.
    //
    // Note also that this creates an "unloaded" skeleton and is therefore
    // relatively cheap.
    //
    package_skeleton (const common_options& co,
                      database&,
                      const available_package&,
                      strings config_vars,
                      bool disfigure,
                      const vector<config_variable>* config_srcs,
                      optional<dir_path> src_root,
                      optional<dir_path> out_root);


    package_key key;
    reference_wrapper<const available_package> available;

    const package_name&
    name () const {return key.name;} // @@ TMP: get rid (use key.name).

    // The following functions should be called in the following sequence
    // (* -- zero or more, ? -- zero or one):
    //
    // * reload_defaults()
    // * verify_sensible()
    // ? dependent_config()
    // * evaluate_enable() | evaluate_reflect()
    //   collect_config()
    //
    // Note that a copy of the skeleton is expected to continue with the
    // sequence rather than starting from scratch.
    //
  public:
    // Reload the default values and type information for configuration
    // variables using the values with the buildfile origin as a "tentative"
    // dependent configuration.
    //
    void
    reload_defaults (package_configuration&);

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
    // to the specified (by index) depends value (used to print its location
    // on failure for an external package).
    //

    // Evaluate the enable clause.
    //
    bool
    evaluate_enable (const string&, size_t depends_index);

    // Evaluate the reflect clause.
    //
    void
    evaluate_reflect (const string&, size_t depends_index);

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
                            const string&, const string&, size_t depends_index);

    bool
    evaluate_require (const dependency_configurations&,
                      const string&, size_t depends_index);

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

    // Implementation details.
    //
  public:
    // We have to define these because context is forward-declared. Also, copy
    // constructor has some special logic.
    //
    ~package_skeleton ();
    package_skeleton (package_skeleton&&);
    package_skeleton& operator= (package_skeleton&&);

    package_skeleton (const package_skeleton&);
    package_skeleton& operator= (const package_skeleton&) = delete;

  private:
    // Load old user configuration variables from config.build (or equivalent)
    // and merge them into config_vars_.
    //
    // This should be done before any attempt to load the configuration with
    // config.config.disfigure and, if this did not happen, inside
    // collect_config() (since the package will be reconfigured with
    // config.config.disfigure).
    //
    void
    load_old_config ();

    // (Re)load the build system state.
    //
    // Call this function before evaluating every clause.
    //
    // If dependency configurations are specified, then typify the variables
    // and set their values. If defaults is false, then only typify the
    // variables and set overrides without setting the default/buildfile
    // values.
    //
    build2::scope&
    load (const dependency_configurations& = {}, bool defaults = true);

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

    strings config_vars_;
    bool disfigure_;
    const vector<config_variable>* config_srcs_; // NULL if nothing to do or
                                                 // already done.

    dir_path src_root_; // Must be absolute and normalized.
    dir_path out_root_; // If empty, the same as src_root_.

    bool created_ = false;
    bool verified_ = false;
    unique_ptr<build2::context> ctx_;
    build2::scope* rs_ = nullptr;

    // Storage for merged build2_cmd_vars and config_vars_ and extra overrides
    // (like config.config.disfigure). If cache is true, then the existing
    // content can be reused.
    //
    strings cmd_vars_;
    bool cmd_vars_cache_ = false;

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

    using reflect_variable_values = vector<reflect_variable_value>;

    strings dependent_vars_; // Dependent configuration variable overrides.
    strings reflect_vars_;   // Reflect configuration variable overrides.
    string  reflect_frag_;   // Reflect configuration variables fragment.

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
  };
}

#endif // BPKG_PACKAGE_SKELETON_HXX
