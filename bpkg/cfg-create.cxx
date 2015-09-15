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

    dir_path d (o.directory ());
    level4 ([&]{trace << "creating configuration in " << d;});

    // If the directory already exists, make sure it is empty.
    // Otherwise, create it.
    //
    if (exists (d))
    {
      level5 ([&]{trace << "directory " << d << " exists";});

      if (!empty (d))
      {
        level5 ([&]{trace << "directory " << d << " not empty";});

        if (!o.wipe ())
          fail << "directory " << d << " is not empty";

        rm_r (d, false);
      }
    }
    else
    {
      level5 ([&]{trace << "directory " << d << " does not exist";});
      mk_p (d);
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
    dir_path bd (d / dir_path ("build"));
    mk (bd);

    // Write build/bootstrap.build.
    //
    path f (bd / path ("bootstrap.build"));
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
    f = path (d / path ("buildfile"));
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
    {
      cstrings args {"b"};

      // Map verbosity level. If we are running quiet or at level 1,
      // then run build2 quiet. Otherwise, run it at the same level
      // as us.
      //
      string vl;
      if (verb <= 1)
        args.push_back ("-q");
      else if (verb == 2)
        args.push_back ("-v");
      else
      {
        vl = to_string (verb);
        args.push_back ("--verbose");
        args.push_back (vl.c_str ());
      }

      // Add config vars.
      //
      for (const string& v: vars)
        args.push_back (v.c_str ());

      // Add buildspec.
      //
      string bspec ("configure(" + d.string () + "/)");
      args.push_back (bspec.c_str ());

      args.push_back (nullptr);
      run (args);
    }

    // Create the database.
    //
    open (d, true);

    if (verb)
    {
      d.complete ();
      d.normalize ();
      text << "created new configuration in " << d;
    }
  }
}
