// file      : bpkg/pkg-command.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-command.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  pkg_command (const string& cmd,
               const dir_path& c,
               const common_options& o,
               const string& cmd_v,
               const strings& cvars,
               const vector<pkg_command_vars>& ps)
  {
    tracer trace ("pkg_command");

    l4 ([&]{trace << "command: " << cmd;});

    // This one is a bit tricky: we can only update all the packages at once
    // if they don't have any package-specific variables and don't require to
    // change the current working directory to the package directory. But
    // let's try to handle this with the same logic (being clever again).
    //
    string bspec;

    auto run = [&trace, &o, &cvars, &bspec] (const strings& vars = strings ())
    {
      if (!bspec.empty ())
      {
        bspec += ')';
        l4 ([&]{trace << "buildspec: " << bspec;});

        run_b (o,
               verb_b::normal,
               (o.jobs_specified ()
                ? strings ({"-j", to_string (o.jobs ())})
                : strings ()),
               cvars,
               vars,
               bspec);

        bspec.clear ();
      }
    };

    for (const pkg_command_vars& pv: ps)
    {
      if (!pv.vars.empty () || pv.cwd)
        run (); // Run previously collected packages.

      if (bspec.empty ())
      {
        bspec = cmd;

        if (!cmd_v.empty ())
        {
          bspec += "-for-";
          bspec += cmd_v;
        }

        bspec += '(';
      }

      const shared_ptr<selected_package>& p (pv.pkg);

      assert (p->state == package_state::configured);
      assert (p->out_root); // Should be present since configured.

      dir_path out_root (p->effective_out_root (c));
      l4 ([&]{trace << p->name << " out_root: " << out_root;});

      if (bspec.back () != '(')
        bspec += ' ';

      // Use path representation to get canonical trailing slash.
      //
      bspec += '\'';
      bspec += (!pv.cwd ? out_root : current_dir).representation ();
      bspec += '\'';

      if (!pv.vars.empty () || pv.cwd)
      {
        // Run this package, changing the current working directory to the
        // package directory, if requested. Note that we do it this way
        // instead of changing the working directory of the process for
        // diagnostics.
        //
        auto owdg = make_guard (
          [owd = pv.cwd ? change_wd (out_root) : dir_path ()] ()
          {
            if (!owd.empty ())
              change_wd (owd);
          });

        run (pv.vars);
      }
    }

    run ();
  }

  static void
  collect_dependencies (const shared_ptr<selected_package>& p,
                        bool recursive,
                        bool package_cwd,
                        vector<pkg_command_vars>& ps)
  {
    for (const auto& pr: p->prerequisites)
    {
      shared_ptr<selected_package> d (pr.first.load ());

      // The selected package can only be configured if all its dependencies
      // are configured.
      //
      assert (d->state == package_state::configured);

      // Skip configured as system and duplicate dependencies.
      //
      if (d->substate != package_substate::system &&
          find_if (ps.begin (), ps.end (),
                   [&d] (const pkg_command_vars& i) {return i.pkg == d;}) ==
          ps.end ())
      {
        // Note: no package-specific variables (global ones still apply).
        //
        ps.push_back (pkg_command_vars {d,
                                        strings () /* vars */,
                                        package_cwd});

        if (recursive)
          collect_dependencies (d, recursive, package_cwd, ps);
      }
    }
  }

  int
  pkg_command (const string& cmd,
               const configuration_options& o,
               const string& cmd_v,
               bool recursive,
               bool immediate,
               bool all,
               bool package_cwd,
               cli::group_scanner& args)
  {
    tracer trace ("pkg_command");

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    // First sort arguments into the package names and variables.
    //
    strings cvars;
    bool sep (false); // Seen '--'.

    struct pkg_arg
    {
      package_name name;
      strings      vars;
    };
    vector<pkg_arg> pkg_args;

    while (args.more ())
    {
      string a (args.next ());

      // If we see the "--" separator, then we are done parsing common
      // variables.
      //
      if (!sep && a == "--")
      {
        sep = true;
        continue;
      }

      if (!sep && a.find ('=') != string::npos)
      {
        // Make sure this is not a (misspelled) package name with an option
        // group.
        //
        if (args.group ().more ())
          fail << "unexpected options group for variable '" << a << "'";

        cvars.push_back (move (a));
      }
      else
      {
        package_name n (parse_package_name (a, false /* allow_version */));

        // Read package-specific variables.
        //
        strings vars;
        for (cli::scanner& ag (args.group ()); ag.more (); )
        {
          string a (ag.next ());
          if (a.find ('=') == string::npos)
            fail << "unexpected group argument '" << a << "'";

          vars.push_back (move (a));
        }

        pkg_args.push_back (pkg_arg {move (n), move (vars)});
      }
    }

    // Check that options and arguments are consistent.
    //
    // Note that we can as well count on the option names that correspond to
    // the immediate, recursive, and all parameters.
    //
    {
      diag_record dr;

      if (immediate && recursive)
        dr << fail << "both --immediate|-i and --recursive|-r specified";
      else if (all)
      {
        if (!pkg_args.empty ())
          dr << fail << "both --all|-a and package argument specified";
      }
      else if (pkg_args.empty ())
        dr << fail << "package name argument expected";

      if (!dr.empty ())
        dr << info << "run 'bpkg help pkg-" << cmd << "' for more information";
    }

    vector<pkg_command_vars> ps;
    {
      database db (open (c, trace));
      transaction t (db);

      // We need to suppress duplicate dependencies for the recursive command
      // execution.
      //
      session ses;

      auto add = [&ps, recursive, immediate, package_cwd] (
        const shared_ptr<selected_package>& p,
        strings vars)
      {
        ps.push_back (pkg_command_vars {p, move (vars), package_cwd});

        // Note that it can only be recursive or immediate but not both.
        //
        if (recursive || immediate)
          collect_dependencies (p, recursive, package_cwd, ps);
      };

      if (all)
      {
        using query = query<selected_package>;

        query q (query::hold_package &&
                 query::state == "configured" &&
                 query::substate != "system");

        for (shared_ptr<selected_package> p:
               pointer_result (db.query<selected_package> (q)))
        {
          l4 ([&]{trace << *p;});

          add (p, strings ());
        }

        if (ps.empty ())
          info << "nothing to " << cmd;
      }
      else
      {
        for (auto& a: pkg_args)
        {
          shared_ptr<selected_package> p (db.find<selected_package> (a.name));

          if (p == nullptr)
            fail << "package " << a.name << " does not exist in "
                 << "configuration " << c;

          if (p->state != package_state::configured)
            fail << "package " << a.name << " is " << p->state <<
              info << "expected it to be configured";

          if (p->substate == package_substate::system)
            fail << "cannot " << cmd << " system package " << a.name;

          l4 ([&]{trace << *p;});

          add (p, move (a.vars));
        }
      }

      t.commit ();
    }

    pkg_command (cmd, c, o, cmd_v, cvars, ps);

    if (verb && !o.no_result ())
    {
      for (const pkg_command_vars& pv: ps)
        text << cmd << (cmd.back () != 'e' ? "ed " : "d ") << *pv.pkg;
    }

    return 0;
  }
}
