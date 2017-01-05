// file      : bpkg/cfg-create.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/cfg-create>

#include <butl/fdstream>

#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/database>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

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

    // If the directory already exists, make sure it is empty.
    // Otherwise, create it.
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
    strings mods;
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
        // Append .config unless the module name ends with '.', in which case
        // strip it.
        //
        if (a.back () != '.')
          a += ".config";
        else
          a.pop_back ();

        mods.push_back (move (a));
      }
      else
        fail << "empty string as argument";
    }

    // Create build/.
    //
    dir_path b (c / dir_path ("build"));
    mk (b);

    // Write build/bootstrap.build.
    //
    path f (b / path ("bootstrap.build"));
    try
    {
      ofdstream ofs (f);

      ofs << "# Maintained automatically by bpkg. Edit if you know what " <<
        "you are doing." << endl
          << "#" << endl
          << "project =" << endl
          << "amalgamation =" << endl
          << endl
          << "using config" << endl
          << "using test" << endl
          << "using install" << endl;

      ofs.close ();
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << f << ": " << e.what ();
    }

    // Write build/root.build.
    //
    f = b / path ("root.build");
    try
    {
      ofdstream ofs (f);

      ofs << "# Maintained automatically by bpkg. Edit if you know what " <<
        "you are doing." << endl
          << "#" << endl;

      // Load user-supplied modules. We don't really know whether they must
      // be loaded in bootstrap.
      //
      for (const string& m: mods)
      {
        // If the module name start with '?', then use optional load.
        //
        if (m.front () != '?')
          ofs << "using " << m << endl;
        else
          ofs << "using? " << m.c_str () + 1 << endl;
      }

      ofs.close ();
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << f << ": " << e.what ();
    }

    // Write root buildfile.
    //
    f = c / path ("buildfile");
    try
    {
      ofdstream ofs (f);

      ofs << "# Maintained automatically by bpkg. Edit if you know what " <<
        "you are doing." << endl
          << "#" << endl
          << "./:" << endl;

      ofs.close ();
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << f << ": " << e.what ();
    }

    // Configure.
    //
    // Run quiet. Use path representation to get canonical trailing slash.
    //
    run_b (o, c, "configure('" + c.representation () + "')", true, vars);

    // Create .bpkg/.
    //
    mk (c / bpkg_dir);

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
