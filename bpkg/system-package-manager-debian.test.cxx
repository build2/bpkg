// file      : bpkg/system-package-manager-debian.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager-debian.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <map>
#include <iostream>

#include <libbutl/manifest-parser.hxx>

#include <libbpkg/manifest.hxx>

#include <bpkg/package.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;

namespace bpkg
{
  using package_status = system_package_status_debian;
  using package_policy = package_status::package_policy;

  using butl::manifest_parser;
  using butl::manifest_parsing;

  // Usage: args[0] <command> ...
  //
  // Where <command> is one of:
  //
  //   apt-cache-policy <pkg>...          result comes from stdin
  //
  //   apt-cache-show <pkg> <ver>         result comes from stdin
  //
  //   parse_name_value                   debian-name value from from stdin
  //
  //   main-from-dev <dev-pkg> <dev-ver>  depends comes from stdin
  //
  //   build <query-pkg>... [--install [--no-fetch] <install-pkg>...]
  //
  // The stdin of the build command is used to read the simulation description
  // which consists of lines in the following forms (blanks are ignored):
  //
  // manifest: <query-pkg> <file>
  //
  //   Available package manifest for one of <query-pkg>. If none is
  //   specified, then a stub is automatically added.
  //
  // apt-cache-policy[-{fetched,installed}]: <sys-pkg>... <file>
  //
  //   Values for simulation::apt_cache_policy_*. If <file> is the special `!`
  //   value, then make the entry empty.
  //
  // apt-cache-show[-fetched]: <sys-pkg> <sys-ver> <file>
  //
  //   Values for simulation::apt_cache_show_*. If <file> is the special `!`
  //   value, then make the entry empty.
  //
  // apt-get-update-fail: true
  // apt-get-install-fail: true
  //
  //   Values for simulation::apt_get_{update,install}_fail_.
  //
  int
  main (int argc, char* argv[])
  try
  {
    assert (argc >= 2); // <command>

    string cmd (argv[1]);

    // @@ TODO: add option to customize? Maybe option before command?
    //
    os_release osr {"debian", {}, "10", "", "Debian", "", ""};

    if (cmd == "apt-cache-policy")
    {
      assert (argc >= 3); // <pkg>...

      strings key;
      vector<package_policy> pps;
      for (int i (2); i != argc; ++i)
      {
        key.push_back (argv[i]);
        pps.push_back (package_policy (argv[i]));
      }

      system_package_manager_debian::simulation s;
      s.apt_cache_policy_.emplace (move (key), path ("-"));

      system_package_manager_debian m (move (osr),
                                       false   /* install */,
                                       false   /* fetch */,
                                       nullopt /* progress */,
                                       false   /* yes */,
                                       "sudo");
      m.simulate_ = &s;

      m.apt_cache_policy (pps);

      for (const package_policy& pp: pps)
      {
        cout << pp.name << " '"
             << pp.installed_version << "' '"
             << pp.candidate_version << "'\n";
      }
    }
    else if (cmd == "apt-cache-show")
    {
      assert (argc == 4); // <pkg> <ver>

      pair<string, string> key (argv[2], argv[3]);

      system_package_manager_debian::simulation s;
      s.apt_cache_show_.emplace (key, path ("-"));

      system_package_manager_debian m (move (osr),
                                       false   /* install */,
                                       false   /* fetch */,
                                       nullopt /* progress */,
                                       false   /* yes */,
                                       "sudo");
      m.simulate_ = &s;

      cout << m.apt_cache_show (key.first, key.second) << '\n';
    }
    else if (cmd == "parse-name-value")
    {
      assert (argc == 2);

      string v;
      getline (cin, v);

      package_status s (
        system_package_manager_debian::parse_name_value (v, false, false));

      if (!s.main.empty ())   cout << "main: "   << s.main   << '\n';
      if (!s.dev.empty ())    cout << "dev: "    << s.dev    << '\n';
      if (!s.doc.empty ())    cout << "doc: "    << s.doc    << '\n';
      if (!s.dbg.empty ())    cout << "dbg: "    << s.dbg    << '\n';
      if (!s.common.empty ()) cout << "common: " << s.common << '\n';
      if (!s.extras.empty ())
      {
        cout << "extras:";
        for (const string& e: s.extras)
          cout << ' ' << e;
        cout << '\n';
      }
    }
    else if (cmd == "main-from-dev")
    {
      assert (argc == 4); // <dev-pkg> <dev-ver>

      string n (argv[2]);
      string v (argv[3]);
      string d;
      getline (cin, d);

      cout << system_package_manager_debian::main_from_dev (n, v, d) << '\n';
    }
    else if (cmd == "build")
    {
      assert (argc >= 3); // <query-pkg>...

      strings qps;
      map<string, available_packages> aps;

      // Parse <query-pkg>...
      //
      int argi (2);
      for (; argi != argc; ++argi)
      {
        string a (argv[argi]);

        if (a.compare (0, 2, "--") == 0)
          break;

        aps.emplace (a, available_packages {});
        qps.push_back (move (a));
      }

      // Parse --install [--no-fetch]
      //
      bool install (false);
      bool fetch (true);

      for (; argi != argc; ++argi)
      {
        string a (argv[argi]);

        if (a == "--install") install = true;
        else if (a == "--no-fetch") fetch = false;
        else break;
      }

      // Parse the description.
      //
      system_package_manager_debian::simulation s;

      for (string l; !eof (getline (cin, l)); )
      {
        if (l.empty ())
          continue;

        size_t p (l.find (':')); assert (p != string::npos);
        string k (l, 0, p);

        if (k == "manifest")
        {
          size_t q (l.rfind (' ')); assert (q != string::npos);
          string n (l, p + 2, q - p - 2); trim (n);
          string f (l, q + 1); trim (f);

          auto i (aps.find (n));
          if (i == aps.end ())
            fail << "unknown package " << n << " in '" << l << "'";

          // Parse the manifest as if it comes from a git repository with a
          // single package and make an available package out of it.
          //
          try
          {
            ifdstream ifs (f);
            manifest_parser mp (ifs, f);

            package_manifest m (mp,
                                false /* ignore_unknown */,
                                true /* complete_values */);

            m.alt_naming = false;
            m.bootstrap_build = "project = " + m.name.string () + '\n';

            shared_ptr<available_package> ap (
              make_shared<available_package> (move (m)));

            lazy_shared_ptr<repository_fragment> af (
              make_shared<repository_fragment> (
                repository_location ("https://example.com/" + i->first,
                                     repository_type::git)));

            ap->locations.push_back (package_location {af, current_dir});

            i->second.push_back (make_pair (move (ap), move (af)));
          }
          catch (const manifest_parsing& e)
          {
            fail (e.name, e.line, e.column) << e.description;
          }
          catch (const io_error& e)
          {
            fail << "unable to read from " << f << ": " << e;
          }
        }
        else if (
          map<strings, path>* policy =
          k == "apt-cache-policy"           ? &s.apt_cache_policy_ :
          k == "apt-cache-policy-fetched"   ? &s.apt_cache_policy_fetched_ :
          k == "apt-cache-policy-installed" ? &s.apt_cache_policy_installed_ :
          nullptr)
        {
          size_t q (l.rfind (' ')); assert (q != string::npos);
          string n (l, p + 2, q - p - 2); trim (n);
          string f (l, q + 1); trim (f);

          strings ns;
          for (size_t b (0), e (0); next_word (n, b, e); )
            ns.push_back (string (n, b, e - b));

          if (f == "!")
            f.clear ();

          policy->emplace (move (ns), path (move (f)));
        }
        else if (map<pair<string, string>, path>* show =
                 k == "apt-cache-show"         ? &s.apt_cache_show_ :
                 k == "apt-cache-show-fetched" ? &s.apt_cache_show_fetched_ :
                 nullptr)
        {
          size_t q (l.rfind (' ')); assert (q != string::npos);
          string n (l, p + 2, q - p - 2); trim (n);
          string f (l, q + 1); trim (f);

          q = n.find (' '); assert (q != string::npos);
          pair<string, string> nv (string (n, 0, q), string (n, q + 1));
          trim (nv.second);

          if (f == "!")
            f.clear ();

          show->emplace (move (nv), path (move (f)));
        }
        else if (k == "apt-get-update-fail")
        {
          s.apt_get_update_fail_ = true;
        }
        else if (k == "apt-get-install-fail")
        {
          s.apt_get_install_fail_ = true;
        }
        else
          fail << "unknown keyword '" << k << "' in simulation description";
      }

      // Fallback to stubs as if they come from git repositories with a single
      // package.
      //
      for (pair<const string, available_packages>& p: aps)
      {
        if (p.second.empty ())
        {
          try
          {
            package_name n (p.first);

            shared_ptr<available_package> ap (
              make_shared<available_package> (move (n)));

            lazy_shared_ptr<repository_fragment> af (
              make_shared<repository_fragment> (
                repository_location ("https://example.com/" + p.first,
                                     repository_type::git)));

            ap->locations.push_back (package_location {af, current_dir});

            p.second.push_back (make_pair (move (ap), move (af)));
          }
          catch (const invalid_argument& e)
          {
            fail << "invalid package name '" << p.first << "': " << e;
          }
        }
      }

      system_package_manager_debian m (move (osr),
                                       install,
                                       fetch,
                                       nullopt /* progress */,
                                       false   /* yes */,
                                       "sudo");
      m.simulate_ = &s;

      // Query each package.
      //
      for (const string& n: qps)
      {
        package_name pn (n);

        const system_package_status* s (*m.pkg_status (pn, &aps[n]));

        if (s == nullptr)
          fail << "no system package for " << pn;

        cout << pn << ' ' << s->version
             << " (" << s->system_name << ' ' << s->system_version << ") ";

        switch (s->status)
        {
        case package_status::installed:           cout << "installed"; break;
        case package_status::partially_installed: cout << "part installed"; break;
        case package_status::not_installed:       cout << "not installed"; break;
        }

        cout << '\n';
      }

      // Install if requested.
      //
      if (install)
      {
        assert (argi != argc); // <install-pkg>...

        vector<package_name> ips;
        for (; argi != argc; ++argi)
          ips.push_back (package_name (argv[argi]));

        m.pkg_install (ips);
      }
    }
    else
      fail << "unknown command '" << cmd << "'";

    return 0;
  }
  catch (const failed&)
  {
    return 1;
  }
}

int
main (int argc, char* argv[])
{
  return bpkg::main (argc, argv);
}
