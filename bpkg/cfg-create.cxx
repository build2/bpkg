// file      : bpkg/cfg-create.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/cfg-create.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;

namespace bpkg
{
  int
  cfg_create (const cfg_create_options& o, cli::scanner& args)
  {
    tracer trace ("cfg_create");

    if (o.wipe () && !o.directory_specified ())
      fail << "--wipe requires explicit --directory|-d";

    dir_path c (o.directory ());
    l4 ([&]{trace << "creating configuration in " << c;});

    // If the directory already exists, make sure it is empty. Otherwise
    // create it.
    //
    if (exists (c))
    {
      l5 ([&]{trace << "directory " << c << " exists";});

      if (!empty (c))
      {
        l5 ([&]{trace << "directory " << c << " not empty";});

        if (!o.wipe ())
          fail << "directory " << c << " is not empty" <<
            info << "use --wipe to clean it up but be careful";

        rm_r (c, false);
      }
    }
    else
    {
      l5 ([&]{trace << "directory " << c << " does not exist";});
      mk_p (c);
    }

    // Sort arguments into modules and configuration variables.
    //
    string mods;  // build2 create meta-operation parameters.
    strings vars;

    while (args.more ())
    {
      string a (args.next ());

      if (a.find ('=') != string::npos)
      {
        vars.push_back (move (a));
      }
      else if (!a.empty ())
      {
        mods += mods.empty () ? ", " : " ";
        mods += a;
      }
      else
        fail << "empty string as argument";
    }

    // Create and configure.
    //
    // Run quiet. Use path representation to get canonical trailing slash.
    //
    run_b (o,
           c,
           "create('" + c.representation () + "'" + mods + ")",
           true,
           vars);

    // Create .bpkg/ and its subdirectories.
    //
    {
      mk (c / bpkg_dir);
      mk (c / certs_dir);
      mk (c / repos_dir);
    }

    // Initialize tmp directory.
    //
    init_tmp (c);

    // Create the database.
    //
    database db (open (c, trace, true));

    // Add the special, root repository object with empty location.
    //
    transaction t (db.begin ());
    db.persist (repository (repository_location ()));
    t.commit ();

    if (verb)
    {
      c.complete ().normalize ();
      text << "created new configuration in " << c;
    }

    return 0;
  }
}
