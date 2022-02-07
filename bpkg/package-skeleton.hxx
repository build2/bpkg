// file      : bpkg/package-skeleton.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PACKAGE_SKELETON_HXX
#define BPKG_PACKAGE_SKELETON_HXX

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
    // non-existing out root directory will be used for build2 state loading
    // instead of the newly created skeleton directory. This, in particular,
    // allows to consider existing configuration variables while evaluating
    // the dependency clauses.
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

      dirty ();
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

  private:
    // Create the skeleton if necessary and (re)load the build system state.
    //
    // Call this function before evaluating every clause.
    //
    void
    load ();

    // Mark the build system state as needing reloading.
    //
    // Call this function after evaluating the reflect clause (since some
    // computed values in root.build may depend on the new value).
    //
    void
    dirty ()
    {
      dirty_ = true;
    }

  private:
    reference_wrapper<database> db_;
    reference_wrapper<const available_package> available_;
    strings config_vars_;

    optional<dir_path> src_root_;
    optional<dir_path> out_root_;

    bool loaded_ = false;
    bool dirty_ = false;

    strings reflect_;
  };
}

#endif // BPKG_PACKAGE_SKELETON_HXX
