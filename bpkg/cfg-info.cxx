// file      : bpkg/cfg-info.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/cfg-info.hxx>

#include <iostream> // cout

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

    database db (c, trace, o.link ());

    try
    {
      cout.exceptions (ostream::badbit | ostream::failbit);

      auto print = [] (const database& db)
      {
        cout << "path: " << db.config                 << endl
             << "uuid: " << db.uuid                   << endl
             << "type: " << db.type                   << endl
             << "name: " << (db.name ? *db.name : "") << endl;
      };

      print (db);

      // Note that there will be no explicit links loaded, unless the --link
      // option is specified.
      //
      for (const linked_config& lc: db.explicit_links ())
      {
        // Skip the self-link.
        //
        if (lc.id != 0)
        {
          cout << endl;
          print (lc.db);
        }
      }
    }
    catch (const io_error&)
    {
      fail << "unable to write to stdout";
    }

    return 0;
  }
}
