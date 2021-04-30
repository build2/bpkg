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

    database db (c, trace, false /* create */, false /* sys_rep */, &ad);
    transaction t (db);

    const dir_path& cd (db.config); // Absolute and normalized.

    // Load the self configuration object from the database of the
    // configuration being associated to obtain its name and type.
    //
    database& adb (db.attach (ad, false /* sys_rep */));
    shared_ptr<configuration> cf (adb.load<configuration> (0));

    // Verify that name and path of the configuration being associated doesn't
    // clash with the already associated configurations.
    //
    optional<string> nm (o.name_specified () ? o.name () : move (cf->name));

    using query = query<configuration>;

    for (auto& ac: db.query<configuration> (query::id != 0))
    {
      // Check for the name clash first (to move the name out afterwords), but
      // report the path clash in the first turn. Indeed, attempt to add the
      // same configuration repeatedly needs to be reported as the "already
      // associated path" error rather than the "already used name".
      //
      bool nc (nm && nm == ac.name);

      string cn (ac.name ? move (*ac.name) : to_string (*ac.id));

      if (ac.path.relative ())
      {
        ac.path = cd / ac.path;

        normalize (ac.path, "associated configuration " + cn);
      }

      if (ad == ac.path)
        fail << "configuration with path '" << ad << "' is already "
             << "associated: " << cn;

      if (nc)
      {
        diag_record dr (fail);
        dr << "configuration with name '" << *nm << "' is already associated";

        if (!o.name_specified ())
          dr << info << "consider specifying name explicitly with --name|-n";
      }
    }

    // Create and persist the associated configuration object.
    //
    string ct (move (cf->type));

    // If the directory path of the configuration being associated is relative
    // or the --relative option is specified, then rebase it against the
    // current configuration directory path.
    //
    // Don't move the type and directory path since they may still be needed.
    //
    cf.reset (new configuration (move (nm), ct, rel ? ad.relative (cd) : ad));
    db.persist (cf);

    // Now associate ourselves with the just associated configuration. Note
    // that we associate ourselves as unnamed if the configuration name
    // clashes.
    //
    cf = db.load<configuration> (0);

    nm = move (cf->name);

    for (auto& ac: adb.query<configuration> (query::id != 0))
    {
      if (nm && nm == ac.name)
        nm = nullopt;

      string cn (ac.name ? move (*ac.name) : to_string (*ac.id));

      if (ac.path.relative ())
      {
        ac.path = ad / ac.path;

        normalize (ac.path, "reverse-associated configuration " + cn);
      }

      if (cd == ac.path)
        fail << "current configuration '" << cd << "' is already "
             << "reverse-associated: " << cn;
    }

    // It feels natural to persist associated configuration paths both either
    // relative or not.
    //
    cf.reset (new configuration (move (nm),
                                 move (cf->type),
                                 rel ? cd.relative (ad) : cd));

    adb.persist (cf);

    t.commit ();

    if (verb)
      text << "associated " << ct << " configuration " << ad;

    return 0;
  }
  catch (const invalid_path& e)
  {
    fail << "invalid path: '" << e.path << "'" << endf;
  }
}
