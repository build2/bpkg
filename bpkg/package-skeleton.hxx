// file      : bpkg/package-skeleton.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PACKAGE_SKELETON_HXX
#define BPKG_PACKAGE_SKELETON_HXX

#include <libbuild2/forward.hxx> // build2::context

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>

namespace bpkg
{
  // A build system skeleton of a package used to evaluate buildfile clauses
  // during dependency resolution (enable, reflect, require or prefer/accept).
  //
  class package_skeleton
  {
  public:
    // If the package is external and will not be disfigured, then the
    // existing package source root directory needs to be specified. In this
    // case this source directory and the automatically deduced potentially
    // non-existent out root directory will be used for build2 state loading
    // instead of the newly created skeleton directory. This, in particular,
    // makes sure we take into account the existing configuration variables
    // while evaluating the dependency clauses (this logic is "parallel" to
    // the configuration preservation in pkg-build.cxx).
    //
    // Note that the database and available_package are expected to outlive
    // this object.
    //
    // Note also that this creates an "unloaded" skeleton and is therefore
    // cheap.
    //
    // @@ Note that storing the list of configuration variables by reference
    //    complicates its use in pkg-build, where both the configuration and
    //    the optional skeleton are parts of the same copyable/moveable
    //    build_package object. We could probably move the configuration into
    //    the skeleton if create it, complicating an access to the
    //    configuration for the users (if the skeleton is present then get
    //    configuration from it, etc). Let's however keep it simple for now
    //    and just copy the configuration.
    //
    package_skeleton (database&,
                      const available_package&,
                      const strings& cvs,
                      optional<dir_path> src_root);

    // Evaluate the enable clause.
    //
    // @@ What can we pass as location? Maybe somehow point to manifest in the
    //    skeleton (will need to re-acquire the position somehow)?
    //
    bool
    evaluate_enable (const string&)
    {
      // @@ TMP
      //
      fail << "conditional dependency for package " << name () <<
        info << "conditional dependencies are not yet supported";

      load ();

      // TODO

      return true; // @@ TMP
    }

    // Evaluate the reflect clause.
    //
    void
    evaluate_reflect (const string& r)
    {
      load ();

      // TODO

      // @@ DEP For now we assume that the reflection, if present, contains
      //    a single configuration variable that assigns a literal value.
      //
      reflect_.push_back (r);

      // Mark the build system state as needing reloading since some computed
      // values in root.build may depend on the new configuration values.
      //
      dirty_ = true;
    }

    // Return the accumulated reflect values.
    //
    strings
    collect_reflect ()
    {
      return reflect_;
    }

    const package_name&
    name () const {return available_.get ().id.name;}

    // Implementation details.
    //
    // We have to define these because context is forward-declared. Also, copy
    // constructor has some special logic.
    //
    ~package_skeleton ();
    package_skeleton (package_skeleton&&);
    package_skeleton& operator= (package_skeleton&&);

    package_skeleton (const package_skeleton&);
    package_skeleton& operator= (package_skeleton&) = delete;

  private:
    // Create the skeleton if necessary and (re)load the build system state.
    //
    // Call this function before evaluating every clause.
    //
    void
    load ();

  private:
    // NOTE: remember to update move/copy constructors!
    //
    reference_wrapper<database> db_;
    reference_wrapper<const available_package> available_;
    strings config_vars_;

    dir_path src_root_;
    dir_path out_root_; // If empty, the same as src_root_.

    unique_ptr<build2::context> ctx_;
    bool created_ = false;
    bool dirty_ = false;

    strings reflect_;
  };
}

#endif // BPKG_PACKAGE_SKELETON_HXX
