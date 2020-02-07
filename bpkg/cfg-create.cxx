// file      : bpkg/cfg-create.cxx -*- C++ -*-
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

    if (o.existing () && o.wipe ())
      fail << "both --existing|-e and --wipe specified";

    if (o.wipe () && !o.directory_specified ())
      fail << "--wipe requires explicit --directory|-d";

    dir_path c (o.directory ());
    l4 ([&]{trace << "creating configuration in " << c;});

    // Verify the existing directory is compatible with our mode.
    //
    if (exists (c))
    {
      if (o.existing ())
      {
        // Bail if the .bpkg/ directory already exists and is not empty.
        //
        // If you are wondering why don't we allow --wipe here, it's the
        // existing packages that may be littering the configuration --
        // cleaning those up will be messy.
        //
        dir_path d (c / bpkg_dir);

        if (exists (d) && !empty (d))
          fail << "directory " << d << " already exists";
      }
      else
      {
        // If the directory already exists, make sure it is empty.
        //
        if (!empty (c))
        {
          if (!o.wipe ())
            fail << "directory " << c << " is not empty" <<
              info << "use --wipe to clean it up but be careful";

          rm_r (c, false);
        }
      }
    }
    else
    {
      // Note that we allow non-existent directory even in the --existing mode
      // in case the user wants to add the build system configuration later.
      //
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
        mods.push_back (move (a));
      }
      else
        fail << "empty string as argument";
    }

    // Create and configure.
    //
    if (o.existing ())
    {
      if (!mods.empty ())
        fail << "module '" << mods[0] << "' specified with --existing|-e";

      if (!vars.empty ())
        fail << "variable '" << vars[0] << "' specified with --existing|-e";
    }
    else
    {
      // Assemble the build2 create meta-operation parameters.
      //
      string params ("'" + c.representation () + "'");
      if (!mods.empty ())
      {
        params += ',';
        for (const string& m: mods)
        {
          params += ' ';
          params += m;
        }
      }

      // Run quiet. Use path representation to get canonical trailing slash.
      //
      run_b (o, verb_b::quiet, vars, "create(" + params + ")");
    }

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
      normalize (c, "configuration");

      if (o.existing ())
        text << "initialized existing configuration in " << c;
      else
        text << "created new configuration in " << c;
    }

    return 0;
  }

  default_options_files
  options_files (const char*, const cfg_create_options& o, const strings&)
  {
    // NOTE: remember to update the documentation if changing anything here.

    // bpkg.options
    // bpkg-cfg-create.options

    // Use the configuration parent directory as a start directory.
    //
    optional<dir_path> start;

    // Let cfg_create() complain later for the root directory used as a
    // configuration directory.
    //
    dir_path d (normalize (o.directory (), "configuration"));
    if (!d.root ())
      start = d.directory ();

    return default_options_files {
      {path ("bpkg.options"), path ("bpkg-cfg-create.options")},
      move (start)};
  }

  cfg_create_options
  merge_options (const default_options<cfg_create_options>& defs,
                 const cfg_create_options& cmd)
  {
    // NOTE: remember to update the documentation if changing anything here.

    return merge_default_options (
      defs,
      cmd,
      [] (const default_options_entry<cfg_create_options>& e,
          const cfg_create_options&)
      {
        const cfg_create_options& o (e.options);

        auto forbid = [&e] (const char* opt, bool specified)
        {
          if (specified)
            fail (e.file) << opt << " in default options file";
        };

        forbid ("--directory|-d", o.directory_specified ());
        forbid ("--wipe",         o.wipe ()); // Dangerous.
      });
  }
}
