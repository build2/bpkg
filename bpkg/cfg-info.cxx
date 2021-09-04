// file      : bpkg/cfg-info.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/cfg-info.hxx>

#include <set>
#include <iostream> // cout

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;

namespace bpkg
{
  int
  cfg_info (const cfg_info_options& o, cli::scanner&)
  {
    tracer trace ("cfg_info");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    if (o.recursive () && !o.link () && !o.backlink ())
      fail << "--recursive requires --link or --backlink";

    try
    {
      cout.exceptions (ostream::badbit | ostream::failbit);

      // Return false if the configuration information has already been
      // printed and print the information and return true otherwise.
      //
      auto print = [first = true,
                    printed = set<dir_path> {}]
        (const dir_path& path,
         const uuid& uid,
         const string& type,
         const optional<string>& name) mutable
      {
        if (!printed.insert (path).second)
          return false;

        if (!first)
          cout << endl;
        else
          first = false;

        cout << "path: " << path                << endl
             << "uuid: " << uid                 << endl
             << "type: " << type                << endl
             << "name: " << (name ? *name : "") << endl;

        return true;
      };

      using query = odb::query<configuration>;

      query q (false);

      if (o.link ())
        q = q || query::expl;

      if (o.backlink () || o.dangling ())
        q = q || (!query::expl && query::id != 0);

      // Make the output consistent across runs.
      //
      q = q + "ORDER BY" + query::id;

      auto print_db = [&o, &q, &print] (database& db,
                                        bool links,
                                        const auto& print_db)
      {
        if (!print (db.config, db.uuid, db.type, db.name))
          return;

        if (links)
        {
          for (auto& c: db.query<configuration> (q))
          {
            const dir_path& d (c.make_effective_path (db.config));

            auto print_link = [&o, &db, &c, &print_db] ()
            {
              database& ldb (db.attach (c.path));
              db.verify_link (c, ldb);

              // While at it, also verify the backlink.
              //
              if (c.expl)
                db.backlink (ldb);

              print_db (ldb, o.recursive (), print_db);
            };

            if (c.expl)
            {
              if (o.link ())
                print_link ();
            }
            else if (exists (d))
            {
              if (o.backlink ())
                print_link ();
            }
            else if (o.dangling ())
              print (d, c.uuid, c.type, c.name);
          }
        }
      };

      database db (c, trace, false /* pre_attach */);
      transaction t (db);

      print_db (db, o.link () || o.backlink () || o.dangling (), print_db);

      t.commit ();
    }
    catch (const io_error&)
    {
      fail << "unable to write to stdout";
    }

    return 0;
  }
}
