// file      : bpkg/cfg-create.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/cfg-create>

#include <utility> // move()
#include <cassert>
#include <fstream>

#include <bpkg/types>
#include <bpkg/utility>
#include <bpkg/database>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  cfg_create (const cfg_create_options& o, cli::scanner& args)
  {
    tracer trace ("cfg_create");

    dir_path c (o.directory ());
    level4 ([&]{trace << "creating configuration in " << c;});

    // If the directory already exists, make sure it is empty.
    // Otherwise, create it.
    //
    if (exists (c))
    {
      level5 ([&]{trace << "directory " << c << " exists";});

      if (!empty (c))
      {
        level5 ([&]{trace << "directory " << c << " not empty";});

        if (!o.wipe ())
          fail << "directory " << c << " is not empty";

        rm_r (c, false);
      }
    }
    else
    {
      level5 ([&]{trace << "directory " << c << " does not exist";});
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

      ofs << "# Maintained automatically by bpkg, do not edit." << endl
          << "#" << endl
          << "project =" << endl
          << "amalgamation =" << endl
          << "using config" << endl
          << "using install" << endl;

      // Load user-supplied modules in bootstrap.build instead of root.build
      // since we don't know whether they can be loaded in the latter.
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
    f = path (c / path ("buildfile"));
    try
    {
      ofstream ofs;
      ofs.exceptions (ofstream::badbit | ofstream::failbit);
      ofs.open (f.string ());

      ofs << "# Maintained automatically by bpkg, do not edit." << endl
          << "#" << endl
          << "./:" << endl;
    }
    catch (const ofstream::failure&)
    {
      fail << "unable to write to " << f;
    }

    // Configure.
    //
    run_b ("configure(" + c.string () + "/)", true, vars); // Run quiet.

    // Create the database.
    //
    open (c, trace, true);

    if (verb)
    {
      c.complete ().normalize ();
      text << "created new configuration in " << c;
    }
  }
}
