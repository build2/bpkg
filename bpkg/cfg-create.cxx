// file      : bpkg/cfg-create.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/cfg-create>

#include <fstream>

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
      (a.find ('=') != string::npos ? vars : mods).push_back (move (a));
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
      ofstream ofs;
      ofs.exceptions (ofstream::badbit | ofstream::failbit);
      ofs.open (f.string ());

      ofs << "# Maintained automatically by bpkg. Edit if you know what " <<
        "you are doing." << endl
          << "#" << endl
          << "project =" << endl
          << "amalgamation =" << endl
          << endl
          << "using config" << endl
          << "using test" << endl
          << "using install" << endl;
    }
    catch (const ofstream::failure&)
    {
      fail << "unable to write to " << f;
    }

    // Write build/root.build.
    //
    f = b / path ("root.build");
    try
    {
      ofstream ofs;
      ofs.exceptions (ofstream::badbit | ofstream::failbit);
      ofs.open (f.string ());

      ofs << "# Maintained automatically by bpkg. Edit if you know what " <<
        "you are doing." << endl
          << "#" << endl;

      // Load user-supplied modules. We don't really know whether they must
      // be loaded in bootstrap.
      //
      for (const string& m: mods)
        ofs << "using " << m << endl;
    }
    catch (const ofstream::failure&)
    {
      fail << "unable to write to " << f;
    }

    // Write root buildfile.
    //
    f = c / path ("buildfile");
    try
    {
      ofstream ofs;
      ofs.exceptions (ofstream::badbit | ofstream::failbit);
      ofs.open (f.string ());

      ofs << "# Maintained automatically by bpkg. Edit if you know what " <<
        "you are doing." << endl
          << "#" << endl
          << "./:" << endl;
    }
    catch (const ofstream::failure&)
    {
      fail << "unable to write to " << f;
    }

    // Configure.
    //
    run_b (o, c, "configure(" + c.string () + "/)", true, vars); // Run quiet.

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
