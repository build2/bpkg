// file      : bpkg/pkg-disfigure.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-disfigure.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  pkg_disfigure (const common_options& o,
                 database& db,
                 transaction& t,
                 const shared_ptr<selected_package>& p,
                 bool clean,
                 bool disfigure,
                 bool simulate)
  {
    assert (p->state == package_state::configured ||
            p->state == package_state::broken);

    tracer trace ("pkg_disfigure");

    l4 ([&]{trace << *p;});

    tracer_guard tg (db, trace);

    // Check that we have no dependents.
    //
    if (p->state == package_state::configured)
    {
      diag_record dr;
      for (database& ddb: db.dependent_configs ())
      {
        auto r (query_dependents (ddb, p->name, db));

        if (!r.empty ())
        {
          if (dr.empty ())
            dr << fail << "package " << p->name << db << " still has "
                       << "dependents:";

          for (const package_dependent& pd: r)
          {
            dr << info << "package " << pd.name << ddb;

            if (pd.constraint)
              dr << " on " << p->name << " " << *pd.constraint;
          }
        }
      }
    }

    if (p->substate == package_substate::system)
    {
      db.erase (p);
      t.commit ();

      p->state = package_state::transient;
      p->substate = package_substate::none;

      return;
    }

    // Since we are no longer configured, clear the prerequisites list.
    //
    p->prerequisites.clear ();
    p->dependency_alternatives.clear ();

    // Mark the section as loaded, so dependency alternatives are updated.
    //
    p->dependency_alternatives_section.load ();

    assert (p->src_root); // Must be set since unpacked.
    assert (p->out_root); // Must be set since configured.

    if (!simulate)
    {
      dir_path src_root (p->effective_src_root (db.config_orig));
      dir_path out_root (p->effective_out_root (db.config_orig));

      l4 ([&]{trace << "src_root: " << src_root << ", "
                    << "out_root: " << out_root;});

      // Form the buildspec.
      //
      string bspec;

      // Use path representation to get canonical trailing slash.
      //
      const string& rep (out_root.representation ());

      if (p->state == package_state::configured)
      {
        if (clean)
          bspec = "clean('" + rep + "')";

        if (disfigure)
        {
          bspec += (bspec.empty () ? "" : " ");
          bspec += "disfigure('" + rep + "')";
        }
      }
      else
      {
        // Why do we need to specify src_root? While it's unnecessary
        // for a completely configured package, here we disfigure a
        // partially configured one.
        //
        if (src_root == out_root)
          bspec = "disfigure('" + rep + "')";
        else
          bspec = "disfigure('" + src_root.representation () + "'@'" + rep +
                  "')";

        disfigure = true; // Make sure the flag matches the behavior.
      }

      // Clean and/or disfigure.
      //
      if (!bspec.empty () && exists (out_root))
      try
      {
        l4 ([&]{trace << "buildspec: " << bspec;});

        // Note that for external packages out_root is only the output
        // directory. It is also possible that the buildfiles in the source
        // directory have changed in a way that they don't clean everything.
        // So in this case we just remove the output directory manually rather
        // then running 'b clean disfigure'.
        //
        // It may also happen that we cannot disfigure the external package'
        // output directory (the source directory have moved, etc.). If that's
        // the case, then we fallback to the output directory removal.
        //
        if (p->external ())
        {
          // clean disfigure
          //
          // true  true  -- wipe the directory
          // true  false -- try to clean, ignore if failed
          // false true  -- try to disfigure, fallback to wipe if failed
          // false false -- never get here (bspec is empty)
          //

          if (!clean || !disfigure)
          {
            auto_fd dev_null (open_null ());

            // Redirect stderr to /dev/null. Note that we don't expect
            // anything to be written to stdout.
            //
            process pr (start_b (o,
                                 1        /* stdout */,
                                 dev_null /* stderr */,
                                 verb_b::quiet,
                                 bspec));

            // If the disfigure meta-operation failed then we report the
            // abnormal termination and fallback to the output directory
            // removal otherwise.
            //
            if (!pr.wait ())
            {
              const process_exit& e (*pr.exit);

              if (!e.normal ())
                fail << "process " << name_b (o) << " " << e;

              clean = true;
            }
          }

          if (clean && disfigure)
            rm_r (out_root);
        }
        else
          run_b (o, verb_b::quiet, bspec);

        // Make sure the out directory is gone unless it is the same as src,
        // or we didn't clean or disfigure it.
        //
        if (out_root != src_root && clean && disfigure && exists (out_root))
          fail << "package output directory " << out_root << " still exists";
      }
      catch (const failed&)
      {
        // If we failed to disfigure the package, set it to the broken
        // state. The user can then try to clean things up with pkg-purge.
        //
        p->state = package_state::broken;
        db.update (p);
        t.commit ();

        info << "package " << p->name << db << " is now broken; "
             << "use 'pkg-purge' to remove";
        throw;
      }

      if (disfigure)
      {
        p->config_variables.clear ();
        p->config_checksum.clear ();
      }
    }

    p->out_root = nullopt;
    p->state = package_state::unpacked;

    db.update (p);
    t.commit ();
  }

  int
  pkg_disfigure (const pkg_disfigure_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_disfigure");

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-disfigure' for more information";

    package_name n (parse_package_name (args.next (),
                                        false /* allow_version */));

    database db (c, trace, true /* pre_attach */);
    transaction t (db);

    shared_ptr<selected_package> p (db.find<selected_package> (n));

    if (p == nullptr)
      fail << "package " << n << " does not exist in configuration " << c;

    if (p->state != package_state::configured)
      fail << "package " << n << " is " << p->state <<
        info << "expected it to be configured";

    // Commits the transaction.
    //
    pkg_disfigure (o, db, t,
                   p,
                   !o.keep_out () /* clean */,
                   !o.keep_config () /* disfigure */,
                   false /* simulate */);

    assert (p->state == package_state::unpacked ||
            p->state == package_state::transient);

    if (verb && !o.no_result ())
      text << (p->state == package_state::transient
               ? "purged "
               : "disfigured ") << *p;

    return 0;
  }
}
