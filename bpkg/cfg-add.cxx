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

    dir_path d (args.next ());
    if (d.empty ())
      throw invalid_path (d.representation ());

    l4 ([&]{trace << "add configuration: " << d;});

    // Load the self configuration object from the database of the
    // configuration being associated to obtain its name and type.
    //
    // @@ Does it makes sense to also symmetrically associate the current
    //    configuration with the configuration being associated? Or should we
    //    do it on demand (when some dependency is built there, etc). The
    //    problem (now and then) is that our name can clash with some other
    //    configuration name there and all we can do, is to associate
    //    ourselves as unnamed.
    //
    shared_ptr<configuration> ac;
    {
      database db (d, trace, false /* create */, false /* sys_rep */);

      transaction t (db);
      ac = db.load<configuration> (0);
      t.commit ();
    }

    database db (c, trace);
    transaction t (db);

    // Verify that name and path of the configuration being associated doesn't
    // clash with other associated configurations.
    //
    optional<string> nm (o.name_specified () ? o.name () : move (ac->name));

    bool rel (d.relative ());
    normalize (d, "specified associated configuration");

    {
      using query = query<configuration>;

      for (auto& ac: db.query<configuration> (query::id != 0))
      {
        if (nm && nm == ac.name)
        {
          diag_record dr (fail);
          dr << "configuration with name '" << *nm << "' is already "
             << "associated";

          if (!o.name_specified ())
            dr << info << "consider specifying name explicitly with --name|-n";
        }

        string cn (ac.name ? move (*ac.name) : "id " + to_string (*ac.id));

        if (ac.path.relative ())
        {
          ac.path = c / ac.path;

          normalize (ac.path,
                     string ("associated configuration " + cn).c_str ());
        }

        if (d == ac.path)
          fail << "configuration with path '" << d << "' is already "
               << "associated: " << cn;
      }
    }

    // Convert the loaded configuration into the associated configuration and
    // persist. Note that the configuration type is already set.
    //
    ac->id   = nullopt;   // Make sure id is assigned automatically.
    ac->name = move (nm);

    // If the directory path of the configuration being associated is relative
    // or the --relative option is specified, then rebase it against the
    // current configuration directory path. Otherwise, normalize it.
    //
    if (rel || o.relative ())
    {
      normalize (c, "configuration");

      ac->path = d.relative (c);
    }
    else
      ac->path = d; // Don't move the path since it may still be needed.

    db.persist (ac);

    t.commit ();

    if (verb)
      text << "associated " << ac->type << " configuration " << d;

    return 0;
  }
  catch (const invalid_path& e)
  {
    fail << "invalid path: '" << e.path << "'" << endf;
  }
}
