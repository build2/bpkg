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
           verb_b::quiet,
           vars,
           "create('" + c.representation () + "'" + mods + ")");

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

    // Add the special, root repository object with empty location and
    // containing a single repository fragment having an empty location as
    // well.
    //
    // Note that the root repository serves as a complement for dir and git
    // repositories that have neither prerequisites nor complements. The
    // root repository fragment is used for transient available package
    // locations and as a search starting point for held packages (see
    // pkg-build for details).
    //
    transaction t (db);

    shared_ptr<repository_fragment> fr (
      make_shared<repository_fragment> (repository_location ()));

    db.persist (fr);

    shared_ptr<repository> r (
      make_shared<repository> (repository_location ()));

    r->fragments.push_back (
      repository::fragment_type {string () /* friendly_name */, move (fr)});

    db.persist (r);

    t.commit ();

    if (verb && !o.no_result ())
    {
      c.complete ().normalize ();
      text << "created new configuration in " << c;
    }

    return 0;
  }
}
