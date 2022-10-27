// file      : bpkg/cfg-create.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/cfg-create.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

#include <bpkg/cfg-link.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  shared_ptr<configuration>
  cfg_create (const common_options& o,
              const dir_path& c,
              optional<string> name,
              string type,
              const strings& mods,
              const strings& vars,
              bool existing,
              bool wipe,
              optional<uuid> uid,
              const optional<dir_path>& host_config,
              const optional<dir_path>& build2_config)
  {
    tracer trace ("cfg_create");

    // Stash and restore the current transaction, if any.
    //
    namespace sqlite = odb::sqlite;

    sqlite::transaction* ct (nullptr);
    if (sqlite::transaction::has_current ())
    {
      ct = &sqlite::transaction::current ();
      sqlite::transaction::reset_current ();
    }

    auto tg (make_guard ([ct] ()
                         {
                           if (ct != nullptr)
                             sqlite::transaction::current (*ct);
                         }));

    // First, let's verify the host/build2 configurations existence and types
    // and normalize their paths.
    //
    auto norm = [&trace] (const dir_path& d, const string& t)
    {
      dir_path r (normalize (d, string (t + " configuration").c_str ()));
      database db (r, trace, false /* pre_attach */);
      if (db.type != t)
        fail << t << " configuration " << r << " is of '" << db.type
             << "' type";

      return r;
    };

    optional<dir_path> hc (host_config
                           ? norm (*host_config, host_config_type)
                           : optional<dir_path> ());

    optional<dir_path> bc (build2_config
                           ? norm (*build2_config, build2_config_type)
                           : optional<dir_path> ());

    // Verify the existing directory is compatible with our mode.
    //
    if (exists (c))
    {
      if (existing)
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
          if (!wipe)
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

    // Create and configure.
    //
    if (existing)
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
      string params ('\'' + c.representation () + '\'');
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
      run_b (o, verb_b::quiet, vars, "create(" + params + ')');
    }

    // Create .bpkg/ and its subdirectories.
    //
    {
      dir_path d (c / bpkg_dir);

      mk (d);
      mk (c / certs_dir);
      mk (c / repos_dir);

      // Create the .gitignore file that ignores everything under .bpkg/
      // effectively making git ignore it (this prevents people from
      // accidentally adding this directory to a git repository).
      //
      path f (d / ".gitignore");
      try
      {
        ofdstream os (f);
        os << "# This directory should not be version-controlled." << '\n'
           << "#"                                                  << '\n'
           << "*"                                                  << '\n';
        os.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << f << ": " << e;
      }
    }

    // Initialize tmp directory.
    //
    init_tmp (c);

    // Create the database.
    //
    shared_ptr<configuration> r (make_shared<configuration> (move (name),
                                                             move (type),
                                                             uid));

    dir_paths pre_link;

    if (hc)
      pre_link.push_back (*hc);

    if (bc)
      pre_link.push_back (*bc);

    database db (c, r, trace, pre_link);
    transaction t (db);

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
    shared_ptr<repository_fragment> fr (
      make_shared<repository_fragment> (repository_location ()));

    db.persist (fr);

    shared_ptr<repository> rep (
      make_shared<repository> (repository_location ()));

    rep->fragments.push_back (
      repository::fragment_type {string () /* friendly_name */, move (fr)});

    db.persist (rep);

    if (hc)
      cfg_link (db, *hc, host_config->relative (), nullopt /* name */);

    if (bc)
      cfg_link (db, *bc, build2_config->relative (), nullopt /* name */);

    t.commit ();

    return r;
  }

  int
  cfg_create (const cfg_create_options& o, cli::scanner& args)
  {
    tracer trace ("cfg_create");

    if (o.name_specified ())
      validate_configuration_name (o.name (), "--name option value");

    if (o.type ().empty ())
      fail << "empty --type option value";

    if (o.existing () && o.wipe ())
      fail << "both --existing|-e and --wipe specified";

    if (o.wipe () && !o.directory_specified ())
      fail << "--wipe requires explicit --directory|-d";

    dir_path c (o.directory ());
    l4 ([&]{trace << "creating configuration in " << c;});

    // Sort arguments into modules and configuration variables.
    //
    strings mods;
    strings vars;
    while (args.more ())
    {
      string a (args.next ());

      if (a.find ('=') != string::npos)
        vars.push_back (move (trim (a)));
      else if (!a.empty ())
        mods.push_back (move (a));
      else
        fail << "empty string as argument";
    }

    // Auto-generate the configuration UUID, unless it is specified
    // explicitly.
    //
    shared_ptr<configuration> cf (
      cfg_create (
        o,
        c,
        o.name_specified () ? o.name () : optional<string> (),
        o.type (),
        mods,
        vars,
        o.existing (),
        o.wipe (),
        o.uuid_specified () ? o.uuid () : optional<uuid> (),
        (o.host_config_specified () && !o.no_host_config ()
         ? o.host_config ()
         : optional<dir_path> ()),
        (o.build2_config_specified () && !o.no_build2_config ()
         ? o.build2_config ()
         : optional<dir_path> ())));

    if (verb && !o.no_result ())
    {
      normalize (c, "configuration");

      diag_record dr (text);

      if (o.existing ())
        dr << "initialized existing configuration in " << c << '\n';
      else
        dr << "created new configuration in " << c << '\n';

      dr << "  uuid: " << cf->uuid << '\n'
         << "  type: " << cf->type;

      if (cf->name)
        dr << "\n  name: " << *cf->name;
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
