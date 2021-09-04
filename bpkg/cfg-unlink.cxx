// file      : bpkg/cfg-unlink.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/cfg-unlink.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;

namespace bpkg
{
  static int
  cfg_unlink_config (const cfg_unlink_options& o, cli::scanner& args)
  try
  {
    tracer trace ("cfg_unlink_config");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    database mdb (c, trace, true /* pre_attach */);
    transaction t (mdb);

    // Find the configuration to be unlinked.
    //
    // Note that we exclude the current configuration from the search.
    //
    database& udb (o.name_specified () ? mdb.find_attached (o.name (), false) :
                   o.id_specified ()   ? mdb.find_attached (o.id (),   false) :
                   o.uuid_specified () ? mdb.find_attached (o.uuid (), false) :
                   mdb.find_attached (
                     normalize (dir_path (args.next ()),
                                "specified linked configuration"),
                     false));

    l4 ([&]{trace << "unlink configuration: " << udb.config;});

    bool priv (udb.private_ ());

    // If the configuration being unlinked contains any prerequisites of
    // packages in other configurations, make sure that they will stay
    // resolvable for their dependents after the configuration is unlinked
    // (see _selected_package_ref::to_ptr() for the resolution details).
    //
    // Specifically, if the configuration being unlinked is private, make sure
    // it doesn't contain any prerequisites of any dependents in any other
    // configurations (since we will remove it). Otherwise, do not consider
    // those dependent configurations which will still be linked with the
    // unlinked configuration (directly or indirectly through some different
    // path).
    //
    // So, for example, for the following link chain where cfg1 contains a
    // dependent of a prerequisite in cfg3, unlinking cfg3 from cfg2 will
    // result with the "cfg3 still depends on cfg1" error.
    //
    // cfg1 (target) -> cfg2 (target) -> cfg3 (host)
    //
    {
      // Note: needs to come before the subsequent unlinking.
      //
      // Also note that this call also verifies integrity of the implicit
      // links of the configuration being unlinked, which we rely upon below.
      //
      linked_databases dcs (udb.dependent_configs ());

      // Unlink the configuration in the in-memory model, so we can evaluate
      // if the dependent configurations are still linked with it.
      //
      // Note that we don't remove the backlink here, since this is not
      // required for the check.
      //
      if (!priv)
      {
        linked_configs& ls (mdb.explicit_links ());

        auto i (find_if (ls.begin (), ls.end (),
                         [&udb] (const linked_config& lc)
                         {
                           return lc.db == udb;
                         }));

        assert (i != ls.end ()); // By definition.

        ls.erase (i);
      }

      // Now go through the packages configured in the unlinked configuration
      // and check it they have some dependents in other configurations which
      // now unable to resolve them as prerequisites. Issue diagnostics and
      // fail if that's the case.
      //
      using query = query<selected_package>;

      for (shared_ptr<selected_package> sp:
             pointer_result (
               udb.query<selected_package> (query::state == "configured")))
      {
        for (auto i (dcs.begin_linked ()); i != dcs.end (); ++i)
        {
          database& db (*i);

          odb::result<package_dependent> ds (
            query_dependents (db, sp->name, udb));

          // Skip the dependent configuration if it doesn't contain any
          // dependents of the package.
          //
          if (ds.empty ())
            continue;

          // Skip the dependent configuration if it is still (potentially
          // indirectly) linked with the unlinked configuration.
          //
          if (!priv)
          {
            linked_databases cs (db.dependency_configs ());

            if (find_if (cs.begin (), cs.end (),
                         [&udb] (const database& db)
                         {
                           return db == udb;
                         }) != cs.end ())
              continue;
          }

          diag_record dr (fail);

          dr << "configuration " << db.config_orig
             << " still depends on " << (priv ? "private " : "")
             << "configuration " << udb.config_orig <<
            info << "package " << sp->name << udb << " has dependents:";

          for (const package_dependent& pd: ds)
          {
            dr << info << "package " << pd.name << db;

            if (pd.constraint)
              dr << " on " << sp->name << " " << *pd.constraint;
          }
        }
      }
    }

    // Now unlink the configuration for real, in the database.
    //
    // Specifically, load the current and the being unlinked configurations
    // and remove their respective explicit and implicit links.
    //
    {
      using query = query<configuration>;

      // Explicit link.
      //
      shared_ptr<configuration> uc (
        mdb.query_one<configuration> (query::uuid == udb.uuid.string ()));

      // The integrity of the current configuration explicit links is verified
      // by the database constructor.
      //
      assert (uc != nullptr);

      // Implicit backlink.
      //
      shared_ptr<configuration> cc (
        udb.query_one<configuration> (query::uuid == mdb.uuid.string ()));

      // The integrity of the implicit links of the configuration being
      // unlinked is verified by the above dependent_configs() call.
      //
      assert (cc != nullptr);

      // If the backlink turns out to be explicit, then, unless the
      // configuration being unlinked is private, we just turn the explicit
      // link into an implicit one rather then remove the direct and back
      // links.
      //
      if (cc->expl && !priv)
      {
        info << "configurations " << udb.config_orig << " and "
             << mdb.config_orig << " are mutually linked, turning the link "
             << "to " << udb.config_orig << " into implicit backlink";

        uc->expl = false;
        mdb.update (uc);
      }
      else
      {
        mdb.erase (uc);
        udb.erase (cc);
      }
    }

    t.commit ();

    // If the unlinked configuration is private, then detach its database and
    // remove its directory. But first, stash the directory path for the
    // subsequent removal and diagnostics.
    //
    dir_path ud (udb.config);

    if (priv)
    {
      mdb.detach_all ();
      rm_r (ud);
    }

    if (verb && !o.no_result ())
      text << "unlinked " << (priv ? "and removed " : "") << "configuration "
           << ud;

    return 0;
  }
  catch (const invalid_path& e)
  {
    fail << "invalid path: '" << e.path << "'" << endf;
  }

  static int
  cfg_unlink_dangling (const cfg_unlink_options& o, cli::scanner&)
  {
    tracer trace ("cfg_unlink_dangling");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    database db (c, trace, false /* pre_attach */);
    transaction t (db);

    using query = query<configuration>;

    size_t count (0);
    for (auto& c: db.query<configuration> (query::id != 0 && !query::expl))
    {
      if (!exists (c.effective_path (db.config)))
      {
        if (verb > 1)
          text << "removing dangling implicit backlink " << c.path;

        db.erase (c);
        ++count;
      }
    }

    t.commit ();

    if (verb && !o.no_result ())
      text << "removed " << count << " dangling implicit backlink(s)";

    return 0;
  }

  int
  cfg_unlink (const cfg_unlink_options& o, cli::scanner& args)
  {
    // Verify that the unlink mode is specified unambiguously.
    //
    // Points to the mode, if any is specified and NULL otherwise.
    //
    const char* mode (nullptr);

    // If the mode is specified, then check that it hasn't been specified yet
    // and set it, if that's the case, or fail otherwise.
    //
    auto verify = [&mode] (const char* m, bool specified)
    {
      if (specified)
      {
        if (mode == nullptr)
          mode = m;
        else
          fail << "both " << mode << " and " << m << " specified";
      }
    };

    verify ("--dangling",         o.dangling ());
    verify ("--name",             o.name_specified ());
    verify ("--id",               o.id_specified ());
    verify ("--uuid",             o.uuid_specified ());
    verify ("directory argument", args.more ());

    if (mode == nullptr)
      fail << "expected configuration to unlink or --dangling option" <<
        info << "run 'bpkg help cfg-unlink' for more information";

    return o.dangling ()
           ? cfg_unlink_dangling (o, args)
           : cfg_unlink_config   (o, args);
  }
}
