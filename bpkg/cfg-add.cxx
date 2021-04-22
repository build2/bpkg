// file      : bpkg/cfg-add.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/cfg-add.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;

namespace bpkg
{
  shared_ptr<configuration>
  cfg_add (database& db,
           const dir_path& ad,
           bool rel,
           optional<string> name,
           bool sys_rep)
  {
    tracer trace ("cfg_add");

    bool name_specified (name);
    const dir_path& cd (db.config); // Note: absolute and normalized.

    // Load the self-association object from the database of the configuration
    // being associated to obtain its name, type, and uuid.
    //
    database& adb (db.attach (ad, sys_rep));

    string type;
    uuid uid;
    {
      shared_ptr<configuration> cf (adb.load<configuration> (0));

      type = move (cf->type);
      uid  = cf->uuid;

      if (!name)
        name = move (cf->name);
    }

    if (db.uuid == uid)
      fail << "associating configuration " << ad << " with itself" <<
        info << "uuid: " << uid;

    if (name && name == db.name)
      fail << "associating configuration " << ad << " using current "
           << "configuration name '" << *name << "'" <<
        info << "consider specifying alternative name with --name";

    // Verify that the name and path of the configuration being associated do
    // not clash with already associated configurations. Fail if
    // configurations with this uuid is already associated unless the
    // association is implicit, in which case make it explicit and update its
    // name and path.
    //
    // Note that when we make an implicit association explicit, we start
    // treating it as an implicit and explicit simultaneously. So, for
    // example, for cfg1 the association cfg2 is explicit and the association
    // cfg3 is both explicit and implicit:
    //
    // cfg2 <- cfg1 <-> cfg3
    //
    // Similar, if we associate cfg1 with cfg2, the explicit association cfg2
    // in cfg1 also becomes both explicit and implicit, not being amended
    // directly.
    //
    shared_ptr<configuration> acf;

    using query = query<configuration>;

    for (shared_ptr<configuration> ac:
           pointer_result (db.query<configuration> (query::id != 0)))
    {
      if (uid == ac->uuid)
      {
        if (ac->expl)
          fail << "configuration with uuid " << uid << " is already "
               << "associated as " << ac->path;

        // Verify the existing implicit association integrity and cache it to
        // update later, when the name/path clash check is complete.
        //
        db.verify_association (*ac, adb);

        acf = move (ac);
        continue;
      }

      if (ad == ac->effective_path (cd))
        fail << "configuration with path " << ad << " is already associated";

      // If the name clashes, then fail if it was specified by the user and
      // issue a warning and associate the configuration as unnamed otherwise.
      //
      if (name && name == ac->name)
      {
        diag_record dr (name_specified ? error : warn);
        dr << "configuration with name " << *name << " is already "
           << "associated as " << ac->path;

        if (name_specified)
        {
          dr << info << "consider specifying alternative name with --name"
             << endf;
        }
        else
        {
          dr << ", associating as unnamed";
          name = nullopt;
        }
      }
    }

    // If requested, rebase the first path relative to the second or return it
    // as is otherwise. Fail if the rebase is not possible (e.g., paths are on
    // different drives on Windows).
    //
    auto rebase = [rel] (const dir_path& x, const dir_path& y) -> dir_path
    {
      try
      {
        return rel ? x.relative (y) : x;
      }
      catch (const invalid_path&)
      {
        fail << "unable to rebase " << x << " relative to " << y <<
          info << "specify absolute configuration directory path to save it "
               << "as absolute" << endf;
      }
    };

    // If the implicit association already exists, then make it explicit and
    // update its name and path. Otherwise, create a new association.
    //
    // Note that in the former case the current configuration must already be
    // explicitly associated with the configuration being associated. We
    // verify that and the association integrity.
    //
    if (acf != nullptr)
    {
      // Verify the reverse association integrity.
      //
      shared_ptr<configuration> cf (
        adb.query_one<configuration> (query::uuid == db.uuid.string ()));

      // Note: both sides of the association cannot be implicit.
      //
      if (cf == nullptr || !cf->expl)
        fail << "configuration " << ad << " is already implicitly "
             << "associated but current configuration " << cd << " is not "
             << "explicitly associated with it";

      adb.verify_association (*cf, db);

      // Finally, turn the implicit association into explicit.
      //
      // Note: reuse id.
      //
      acf->expl = true;
      acf->name = move (name);
      acf->path = rebase (ad, cd); // Note: can't clash (see above).

      db.update (acf);
    }
    else
    {
      // If the directory path of the configuration being associated is
      // relative or the --relative option is specified, then rebase it
      // relative to the current configuration directory path.
      //
      acf = make_shared<configuration> (uid,
                                        move (name),
                                        move (type),
                                        rebase (ad, cd),
                                        true /* explicit */);

      db.persist (acf);

      // Now implicitly associate ourselves with the just associated
      // configuration. Note that we associate ourselves as unnamed.
      //
      shared_ptr<configuration> ccf (db.load<configuration> (0));

      // What if we find the current configuration to already be implicitly
      // associated? The potential scenario could be, that the current
      // configuration was recreated from scratch, previously being implicitly
      // associated with the configuration we currently associate. It feels
      // like in this case we would rather overwrite the existing dead
      // implicit association than just fail. Let's also warn for good
      // measure.
      //
      shared_ptr<configuration> cf;

      for (shared_ptr<configuration> ac:
             pointer_result (adb.query<configuration> (query::id != 0)))
      {
        if (cd == ac->make_effective_path (ad))
        {
          if (ac->expl)
            fail << "current configuration " << cd << " is already "
                 << "associated with " << ad;

          warn << "current configuration " << cd << " is already "
               << "implicitly associated with " << ad;

          cf = move (ac);
          continue;
        }

        if (ccf->uuid == ac->uuid)
          fail << "current configuration " << ccf->uuid
               << " is already associated with " << ad;
      }

      // It feels natural to persist explicitly and implicitly associated
      // configuration paths both either relative or absolute.
      //
      if (cf != nullptr)
      {
        // The dead implicit association case.
        //
        // Note: reuse id.
        //
        cf->uuid = ccf->uuid;
        cf->type = move (ccf->type);
        cf->path = rebase (cd, ad);

        adb.update (cf);
      }
      else
      {
        ccf = make_shared<configuration> (ccf->uuid,
                                          nullopt /* name */,
                                          move (ccf->type),
                                          rebase (cd, ad),
                                          false /* explicit */);

        adb.persist (ccf);
      }
    }

    // If explicit associations of the current database are pre-attached, then
    // also pre-attach explicit associations of the newly associated database.
    //
    associated_configs& acs (db.explicit_associations ());

    if (!acs.empty ())
    {
      acs.push_back (associated_config {*acf->id, acf->name, adb});
      adb.attach_explicit (sys_rep);
    }

    // If the implicit associations of the added database are already
    // attached, then also attach the current database, unless it is already
    // there (see above for the dead association case).
    //
    associated_databases& ads (adb.implicit_associations (false /* attach */));

    if (!ads.empty () && find (ads.begin (), ads.end (), db) == ads.end ())
      ads.push_back (db);

    return acf;
  }

  int
  cfg_add (const cfg_add_options& o, cli::scanner& args)
  try
  {
    tracer trace ("cfg_add");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    if (o.name_specified ())
      validate_configuration_name (o.name (), "--name option value");

    if (!args.more ())
      fail << "configuration directory argument expected" <<
        info << "run 'bpkg help cfg-add' for more information";

    dir_path ad (args.next ());
    if (ad.empty ())
      throw invalid_path (ad.string ());

    l4 ([&]{trace << "add configuration: " << ad;});

    bool rel (ad.relative () || o.relative ());
    normalize (ad, "specified associated configuration");

    database db (c, trace, false /* pre_attach */, false /* sys_rep */, &ad);
    transaction t (db);

    shared_ptr<configuration> ac (
      cfg_add (db,
               ad,
               rel,
               o.name_specified () ? o.name () : optional<string> ()));

    t.commit ();

    if (verb && !o.no_result ())
    {
      diag_record dr (text);

      dr << "associated configuration " << ad <<
        info << "uuid: " << ac->uuid <<
        info << "type: " << ac->type;

      if (ac->name)
        dr << info << "name: " << *ac->name;

      dr << info << "id:   " << *ac->id;
    }

    return 0;
  }
  catch (const invalid_path& e)
  {
    fail << "invalid path: '" << e.path << "'" << endf;
  }
}
