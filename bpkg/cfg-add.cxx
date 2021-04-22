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
  int
  cfg_add (const cfg_add_options& o, cli::scanner& args)
  try
  {
    tracer trace ("cfg_add");

    if (o.name_specified ())
      validate_configuration_name (o.name (), "--name|-n option value");

    if (!args.more ())
      fail << "configuration directory argument expected" <<
        info << "run 'bpkg help cfg-add' for more information";

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    dir_path ad (args.next ());
    if (ad.empty ())
      throw invalid_path (ad.representation ());

    l4 ([&]{trace << "add configuration: " << ad;});

    bool rel (ad.relative () || o.relative ());
    normalize (ad, "specified associated configuration");

    database db (c, trace, false /* pre_attach */, false /* sys_rep */, &ad);
    transaction t (db);

    const dir_path& cd (db.config); // Note: absolute and normalized.

    // Load the self configuration object from the database of the
    // configuration being associated to obtain its name, type, and uuid.
    //
    database& adb (db.attach (ad, false /* sys_rep */));

    optional<string> name;
    string type;
    uuid uid;
    {
      shared_ptr<configuration> cf (adb.load<configuration> (0));
      name = o.name_specified () ? o.name () : move (cf->name);
      type = move (cf->type);
      uid  = cf->uuid;
    }

    // Verify that the name and path of the configuration being associated do
    // not clash with already associated configurations. Fail if
    // configurations with this uuid is already associated unless the
    // association is implicit, in which case make it explicit and update its
    // name and path.
    //
    shared_ptr<configuration> acf;

    using query = query<configuration>;

    for (shared_ptr<configuration> ac:
           pointer_result (db.query<configuration> (query::id != 0)))
    {
      if (uid == ac->uuid)
      {
        if (ac->expl)
          fail << "configuration " << uid.string () << " is already "
               << "associated";

        // Just cache the implicit association and update it later, when the
        // name/path check is complete. But first make sure its type still
        // matches.
        //
        if (type != ac->type)
          fail << "configuration type '" << type << "' doesn't match "
               << "existing association type '" << ac->type << "'";

        acf = move (ac);
        continue;
      }

      if (ad == ac->resolve_path (cd))
        fail << "configuration '" << ad << "' is already associated: "
             << ac->uuid.string ();

      if (name && name == ac->name)
      {
        diag_record dr (fail);
        dr << "configuration named '" << *name << "' is already associated: "
           << ac->uuid.string ();

        if (!o.name_specified ())
          dr << info << "consider specifying name explicitly with --name|-n";
      }
    }

    // If the implicit association already exists, then make it explicit and
    // update its name and path. Otherwise, create the new association.
    //
    // Note that in the former case we assume that the current configuration
    // is already associated with the configuration being associated.
    //
    // Don't move the associated configuration absolute path (ad) since it
    // still be needed.
    //
    if (acf != nullptr)
    {
      acf->expl = true;
      acf->name = move (name);
      acf->path = rel ? ad.relative (cd) : ad;

      db.update (acf);
    }
    else
    {
      // If the directory path of the configuration being associated is
      // relative or the --relative option is specified, then rebase it
      // against the current configuration directory path.
      //
      acf = make_shared<configuration> (move (name),
                                        move (type),
                                        rel ? ad.relative (cd) : ad,
                                        true /* explicit */,
                                        uid);

      db.persist (acf);

      // Now implicitly associate ourselves with the just associated
      // configuration. Note that we associate ourselves as unnamed if the
      // configuration name clashes.
      //
      shared_ptr<configuration> cf (db.load<configuration> (0));

      name = move (cf->name);

      for (auto& ac: adb.query<configuration> (query::id != 0))
      {
        if (cf->uuid == ac.uuid)
          fail << "current configuration " << cf->uuid.string ()
               << " is already associated with '" << ad << "'";

        if (cd == ac.resolve_path (ad))
          fail << "current configuration '" << cd << "' is already "
               << "associated with '" << ad << "'";

        if (name && name == ac.name)
          name = nullopt;
      }

      // It feels natural to persist associated configuration paths both either
      // relative or not.
      //
      cf = make_shared<configuration> (move (name),
                                       move (cf->type),
                                       rel ? cd.relative (ad) : cd,
                                       false /* explicit */,
                                       cf->uuid);

      adb.persist (cf);
    }

    t.commit ();

    if (verb)
      text << "associated " << acf->type << " configuration " << ad;

    return 0;
  }
  catch (const invalid_path& e)
  {
    fail << "invalid path: '" << e.path << "'" << endf;
  }
}
