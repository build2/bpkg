// file      : bpkg/pkg-checkout.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-checkout.hxx>

#include <libbutl/sha256.mxx>

#include <libbpkg/manifest.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/pkg-purge.hxx>
#include <bpkg/pkg-configure.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  shared_ptr<selected_package>
  pkg_checkout (const common_options& o,
                const dir_path& c,
                transaction& t,
                string n,
                version v,
                bool replace)
  {
    tracer trace ("pkg_checkout");

    dir_path d (c / dir_path (n + '-' + v.string ()));

    if (exists (d))
      fail << "package directory " << d << " already exists";

    database& db (t.database ());
    tracer_guard tg (db, trace);

    // See if this package already exists in this configuration.
    //
    shared_ptr<selected_package> p (db.find<selected_package> (n));

    if (p != nullptr)
    {
      bool s (p->state == package_state::fetched ||
              p->state == package_state::unpacked);

      if (!replace || !s)
      {
        diag_record dr (fail);

        dr << "package " << n << " already exists in configuration " << c <<
          info << "version: " << p->version_string ()
           << ", state: " << p->state
           << ", substate: " << p->substate;

        if (s) // Suitable state for replace?
          dr << info << "use 'pkg-checkout --replace|-r' to replace";
      }
    }

    if (db.query_value<repository_count> () == 0)
      fail << "configuration " << c << " has no repositories" <<
        info << "use 'bpkg rep-add' to add a repository";

    if (db.query_value<available_package_count> () == 0)
      fail << "configuration " << c << " has no available packages" <<
        info << "use 'bpkg rep-fetch' to fetch available packages list";

    // Note that here we compare including the revision (see pkg-fetch()
    // implementation for more details).
    //
    shared_ptr<available_package> ap (
      db.find<available_package> (available_package_id (n, v)));

    if (ap == nullptr)
      fail << "package " << n << " " << v << " is not available";

    // Pick a version control-based repository. Preferring a local one over
    // the remotes seems like a sensible thing to do.
    //
    const package_location* pl (nullptr);

    for (const package_location& l: ap->locations)
    {
      const repository_location& rl (l.repository.load ()->location);

      if (rl.version_control_based () && (pl == nullptr || rl.local ()))
      {
        pl = &l;

        if (rl.local ())
          break;
      }
    }

    if (pl == nullptr)
      fail << "package " << n << " " << v
           << " is not available from a version control-based repository";

    if (verb > 1)
      text << "checking out " << pl->location.leaf () << " "
           << "from " << pl->repository->name;

    const repository_location& rl (pl->repository->location);

    // Note: for now we assume this is a git repository. If/when we add other
    // version control-based repositories, this will need adjustment.
    //

    // Currently the git repository state already contains the checked out
    // working tree so all we need to do is distribute it to the package
    // directory.
    //
    dir_path sd (c / repos_dir);

    sd /= dir_path (sha256 (rl.canonical_name ()).abbreviated_string (16));
    sd /= dir_path (pl->state);
    sd /= path_cast<dir_path> (pl->location);

    // Verify the package prerequisites are all configured since the dist
    // meta-operation generally requires all imports to be resolvable.
    //
    pkg_configure_prerequisites (o, t, sd);

    // The temporary out of source directory that is required for the dist
    // meta-operation.
    //
    auto_rmdir rmo (temp_dir / dir_path (n));
    const dir_path& od (rmo.path);

    if (exists (od))
      rm_r (od);

    // Form the buildspec.
    //
    string bspec ("dist(");
    bspec += sd.representation ();
    bspec += '@';
    bspec += od.representation ();
    bspec += ')';

    // Remove the resulting package distribution directory on failure.
    //
    auto_rmdir rmd (d);

    // Distribute.
    //
    // Note that on failure the package stays in the existing (working) state.
    //
    // At first it may seem we have a problem: an existing package with the
    // same name will cause a conflict since we now have multiple package
    // locations for the same package name. We are luck, however: subprojects
    // are only loaded if used and since we don't support dependency cycles,
    // the existing project should never be loaded by any of our dependencies.
    //
    run_b (o,
           c,
           bspec,
           false /* quiet */,
           strings ({"config.dist.root=" + c.representation ()}));

    if (p != nullptr)
    {
      // Clean up the source directory and archive of the package we are
      // replacing. Once this is done, there is no going back. If things go
      // badly, we can't simply abort the transaction.
      //
      pkg_purge_fs (c, t, p);

      p->version = move (v);
      p->state = package_state::unpacked;
      p->repository = rl;
      p->src_root = d.leaf ();
      p->purge_src = true;

      db.update (p);
    }
    else
    {
      // Add the package to the configuration.
      //
      p.reset (new selected_package {
        move (n),
        move (v),
        package_state::unpacked,
        package_substate::none,
        false,     // hold package
        false,     // hold version
        rl,
        nullopt,   // No archive
        false,
        d.leaf (), // Source root.
        true,      // Purge directory.
        nullopt,   // No output directory yet.
        {}});      // No prerequisites captured yet.

      db.persist (p);
    }

    t.commit ();

    rmd.cancel ();
    return p;
  }

  int
  pkg_checkout (const pkg_checkout_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_checkout");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    database db (open (c, trace));
    transaction t (db.begin ());
    session s;

    shared_ptr<selected_package> p;

    if (!args.more ())
      fail << "package name/version argument expected" <<
        info << "run 'bpkg help pkg-checkout' for more information";

    const char* arg (args.next ());
    string n (parse_package_name (arg));
    version v (parse_package_version (arg));

    if (v.empty ())
      fail << "package version expected" <<
        info << "run 'bpkg help pkg-checkout' for more information";

    // Commits the transaction.
    //
    p = pkg_checkout (o, c, t, move (n), move (v), o.replace ());

    if (verb)
      text << "checked out " << *p;

    return 0;
  }
}
