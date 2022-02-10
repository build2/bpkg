// file      : bpkg/package-skeleton.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package-skeleton.hxx>

#include <libbutl/manifest-serializer.hxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>
#include <libbuild2/context.hxx>

#include <bpkg/package.hxx>
#include <bpkg/database.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  // These are defined in bpkg.cxx and initialized in main().
  //
  extern build2::scheduler      build2_sched;
  extern build2::global_mutexes build2_mutexes;
  extern build2::file_cache     build2_fcache;

  package_skeleton::
  ~package_skeleton ()
  {
  }

  package_skeleton::
  package_skeleton (package_skeleton&& v)
      : db_ (v.db_), available_ (v.available_)
  {
    *this = move (v);
  }

  package_skeleton& package_skeleton::
  operator= (package_skeleton&& v)
  {
    if (this != &v)
    {
      db_ = v.db_;
      available_ = v.available_;
      config_vars_ = move (v.config_vars_);
      src_root_ = move (v.src_root_);
      out_root_ = move (v.out_root_);
      ctx_ = move (v.ctx_);
      created_ = v.created_;
      dirty_ = v.dirty_;
      reflect_ = move (v.reflect_);
    }

    return *this;
  }

  package_skeleton::
  package_skeleton (const package_skeleton& v)
      : db_ (v.db_),
        available_ (v.available_),
        config_vars_ (v.config_vars_),
        src_root_ (v.src_root_),
        out_root_ (v.out_root_),
        created_ (v.created_)
  {
    // The idea here is to create an unloaded copy but with enough state that
    // it can be loaded if necessary.

    if (v.ctx_ != nullptr)
    {
      // @@ extract reflection
    }
    else
    {
      // @@ TODO: copy already extracted?
      reflect_ = v.reflect_;
    }
  }

  package_skeleton::
  package_skeleton (database& db,
                    const available_package& ap,
                    const strings& cvs,
                    optional<dir_path> src_root)
      : db_ (db),
        available_ (ap),
        config_vars_ (cvs),
        src_root_ (move (src_root))
  {
    // Should not be created for stubs.
    //
    assert (available_.get ().bootstrap_build);

    if (src_root_)
      out_root_ = dir_path (db_.get ().config_orig) /= name ().string ();
  }

  void package_skeleton::
  load ()
  {
    if (ctx_ != nullptr && !dirty_)
      return;

    // The overall plan is as follows: @@ TODO: revise
    //
    // 0. Create filesystem state if necessary (could have been created by
    //    another instance, e.g., during simulation).
    //
    // 1. If loaded but dirty, save the accumulated reflect state, and
    //    destroy the old state.
    //
    // 2. Load the state potentially with accumulated reflect state.

    // Create the skeleton filesystem state, if it doesn't exist yet.
    //
    if (!created_)
    {
      const available_package& ap (available_);

      // Note that we create the skeleton directories in the skeletons/
      // subdirectory of the configuration temporary directory to make sure
      // they never clash with other temporary subdirectories (git
      // repositories, etc).
      //
      if (!src_root_)
      {
        auto i (temp_dir.find (db_.get ().config_orig));
        assert (i != temp_dir.end ());

        dir_path d (i->second);
        d /= "skeletons";
        d /= name ().string () + '-' + ap.version.string ();

        src_root_ = d;
        out_root_ = move (d);
      }

      if (!exists (*src_root_))
      {
        // Create the buildfiles.
        //
        // Note that it's probably doesn't matter which naming scheme to use
        // for the buildfiles, unless in the future we allow specifying
        // additional files.
        //
        {
          path bf (*src_root_ / std_bootstrap_file);

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
            save (*ap.root_build, *src_root_ / std_root_file);
        }

        // Create the manifest file containing the bare minimum of values
        // which can potentially be required to load the build system state.
        //
        {
          package_manifest m;
          m.name = name ();
          m.version = ap.version;

          // Note that there is no guarantee that the potential build2
          // constraint has already been verified. Thus, we also serialize the
          // depends value, delegating the constraint verification to the
          // version module. Also note that normally the toolchain build-time
          // dependencies are specified first and, if that's the case, their
          // constraints are already verified at this point and so build2 will
          // not fail due to the constraint violation.
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

          path mf (*src_root_ / manifest_file);

          try
          {
            ofdstream os (mf);
            manifest_serializer s (os, mf.string ());
            m.serialize (s);
            os.close ();
          }
          catch (const manifest_serialization&)
          {
            // We shouldn't be creating a non-serializable manifest, since
            // it's crafted from the parsed values.
            //
            assert (false);
          }
          catch (const io_error& e)
          {
            fail << "unable to write to " << mf << ": " << e;
          }
        }
      }

      created_ = true;
    }

    // Creating a new context is not exactly cheap (~1.2ms debug, 0.08ms
    // release) so we could try to re-use it by cleaning all the scopes other
    // than the global scope (and probably some other places, like var pool).
    // But we will need to carefully audit everything to make sure we don't
    // miss anything (like absolute scope variable overrides being lost). So
    // maybe, one day.
    //
    if (dirty_)
    {
      ctx_.reset ();
      dirty_ = false;
    }

    // Create build context.
    //
    // @@ BUILD2_VAR_OVR, environment/default options files? E.g., !config
    //    to find module... Can't we make it reusable in build2?
    //
    // @@ Can we release context memory early, for example, when
    //    collect_reflect() is called?
    //

    // We can reasonably assume reflect cannot have global or absolute scope
    // variable overrides so we don't need to pass them to context.
    //
    using namespace build2;

    ctx_.reset (
      new context (build2_sched,
                   build2_mutexes,
                   build2_fcache,
                   false /* match_only */,          // Shouldn't matter.
                   false /* no_external_modules */,
                   false /* dry_run */,             // Shouldn't matter.
                   false /* keep_going */,          // Shouldnt' matter.
                   config_vars_));
  }
}
