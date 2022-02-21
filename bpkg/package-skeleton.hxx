// file      : bpkg/package-skeleton.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PACKAGE_SKELETON_HXX
#define BPKG_PACKAGE_SKELETON_HXX

#include <libbuild2/forward.hxx> // build2::context

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>
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
    // Note that the options, database, and available_package are expected to
    // outlive this object.
    //
    // Note also that this creates an "unloaded" skeleton and is therefore
    // relatively cheap.
    //
    package_skeleton (const common_options& co,
                      database&,
                      const available_package&,
                      strings config_vars,
                      optional<dir_path> src_root,
                      optional<dir_path> out_root);

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

    // Return the accumulated reflect values.
    //
    // Note that the result is merged with config_vars and you should be used
    // instead rather than in addition to config_vars.
    //
    // Note also that this should be the final call on this object.
    //
    strings
    collect_reflect () &&;

    const package_name&
    name () const {return available_->id.name;}

    // Implementation details.
    //
    // We have to define these because context is forward-declared. Also, copy
    // constructor has some special logic.
    //
    ~package_skeleton ();
    package_skeleton (package_skeleton&&);
    package_skeleton& operator= (package_skeleton&&);

    package_skeleton (const package_skeleton&);
    package_skeleton& operator= (const package_skeleton&) = delete;

  private:
    // Create the skeleton if necessary and (re)load the build system state.
    //
    // Call this function before evaluating every clause.
    //
    build2::scope&
    load ();

  private:
    // NOTE: remember to update move/copy constructors!
    //
    const common_options* co_;
    database* db_;
    const available_package* available_;
    strings config_vars_;

    dir_path src_root_; // Must be absolute and normalized.
    dir_path out_root_; // If empty, the same as src_root_.

    bool created_ = false;
    unique_ptr<build2::context> ctx_;
    build2::scope* rs_ = nullptr;
    strings cmd_vars_; // Storage for merged build2_cmd_vars and config_vars_.

    strings reflect_names_; // Reflect configuration variable names.
    strings reflect_vars_;  // Reflect configuration variable overrides.
    string  reflect_frag_;  // Reflect configuration variable fragment.
  };
}

#endif // BPKG_PACKAGE_SKELETON_HXX
