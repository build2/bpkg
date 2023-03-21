// file      : bpkg/system-package-manager-fedora.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager-fedora.hxx>

#include <map>
#include <iostream>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#undef NDEBUG
#include <cassert>

#include <bpkg/system-package-manager.test.hxx>

using namespace std;

namespace bpkg
{
  using package_status = system_package_status_fedora;
  using package_info_ = package_status::package_info;
  using package = system_package_manager_fedora::simulation::package;

  using butl::manifest_parser;
  using butl::manifest_parsing;

  // Usage: args[0] <command> ...
  //
  // Where <command> is one of:
  //
  //   dnf-list <pkg>...                          result comes from stdin
  //
  //   dnf-repoquery-requires <pkg> <ver> <arc>   result comes from stdin
  //
  //   parse-name-value <pkg>                     fedora-name value from stdin
  //
  //   main-from-devel <dev-pkg> <dev-ver>        depends comes from stdin in
  //                                              the `<dep-pkg> <dep-ver>`
  //                                              per line form
  //
  //   map-package                                manifest comes from stdin
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
  // dnf-list[-{fetched,installed}]: <sys-pkg>... <file>
  //
  //   Values for simulation::dnf_list_*. If <file> is the special `!` value,
  //   then make the entry empty.
  //
  // dnf-repoquery-requires[-fetched]: <sys-pkg> <sys-ver> <sys-arch> <file>
  //
  //   Values for simulation::dnf_repoquery_requires_*. If <file> is the
  //   special `!` value, then make the entry empty.
  //
  // dnf_makecache-fail: true
  // dnf-install-fail: true
  // dnf-mark-install-fail: true
  //
  //   Values for simulation::dnf_{makecache,install,mark_install}_fail_.
  //
  // While creating the system package manager always pretend to be the x86_64
  // Fedora host (x86_64-redhat-linux-gnu), regardless of the actual host
  // platform.
  //
  int
  main (int argc, char* argv[])
  try
  {
    assert (argc >= 2); // <command>

    target_triplet host_triplet ("x86_64-redhat-linux-gnu");

    string cmd (argv[1]);

    // @@ TODO: add option to customize? Maybe option before command?
    //
    os_release osr {"fedora", {}, "35", "", "Fedora Linux", "", ""};

    auto to_bool = [] (const string& s)
    {
      assert (s == "true" || s == "false");
      return s == "true";
    };

    if (cmd == "dnf-list")
    {
      assert (argc >= 3); // <pkg>...

      strings key;
      vector<package_info_> pis;
      for (int i (2); i != argc; ++i)
      {
        key.push_back (argv[i]);
        pis.push_back (package_info_ (argv[i]));
      }

      system_package_manager_fedora::simulation s;
      s.dnf_list_.emplace (move (key), path ("-"));

      system_package_manager_fedora m (move (osr),
                                       host_triplet,
                                       ""      /* arch */,
                                       nullopt /* progress */,
                                       nullopt /* fetch_timeout */,
                                       false   /* install */,
                                       false   /* fetch */,
                                       false   /* yes */,
                                       "sudo");
      m.simulate_ = &s;

      m.dnf_list (pis);

      for (const package_info_& pi: pis)
      {
        cout << pi.name << " '"
             << pi.installed_version << "' '"
             << pi.installed_arch    << "' '"
             << pi.candidate_version << "' '"
             << pi.candidate_arch    << "'\n";
      }
    }
    else if (cmd == "dnf-repoquery-requires")
    {
      assert (argc == 6); // <pkg> <ver> <arch> <installed>

      package key {argv[2], argv[3], argv[4], to_bool (argv[5])};

      system_package_manager_fedora::simulation s;
      s.dnf_repoquery_requires_.emplace (key, path ("-"));

      system_package_manager_fedora m (move (osr),
                                       host_triplet,
                                       ""      /* arch */,
                                       nullopt /* progress */,
                                       nullopt /* fetch_timeout */,
                                       false   /* install */,
                                       false   /* fetch */,
                                       false   /* yes */,
                                       "sudo");
      m.simulate_ = &s;

      for (const pair<string, string>& d:
             m.dnf_repoquery_requires (key.name,
                                       key.version,
                                       key.arch,
                                       key.installed))
      {
        cout << d.first << ' ' << d.second << '\n';
      }
    }
    else if (cmd == "parse-name-value")
    {
      assert (argc == 3); // <pkg>

      package_name pn (argv[2]);
      string pt (package_manifest::effective_type (nullopt, pn));

      string v;
      getline (cin, v);

      package_status s (
        system_package_manager_fedora::parse_name_value (
          pt, v, false, false, false));

      if (!s.main.empty ())        cout << "main: "        << s.main        << '\n';
      if (!s.devel.empty ())       cout << "devel: "       << s.devel       << '\n';
      if (!s.static_.empty ())     cout << "static: "      << s.static_     << '\n';
      if (!s.doc.empty ())         cout << "doc: "         << s.doc         << '\n';
      if (!s.debuginfo.empty ())   cout << "debuginfo: "   << s.debuginfo   << '\n';
      if (!s.debugsource.empty ()) cout << "debugsource: " << s.debugsource << '\n';
      if (!s.common.empty ())      cout << "common: "      << s.common      << '\n';
      if (!s.extras.empty ())
      {
        cout << "extras:";
        for (const string& e: s.extras)
          cout << ' ' << e;
        cout << '\n';
      }
    }
    else if (cmd == "main-from-devel")
    {
      assert (argc == 4); // <dev-pkg> <dev-ver>

      string n (argv[2]);
      string v (argv[3]);
      vector<pair<string, string>> ds;

      for (string l; !eof (getline (cin, l)); )
      {
        size_t p (l.find (' '));
        assert (p != string::npos);

        ds.emplace_back (string (l, 0, p), string (l, p + 1));
      }

      cout << system_package_manager_fedora::main_from_devel (n, v, ds) << '\n';
    }
    else if (cmd == "map-package")
    {
      assert (argc == 2);

      available_packages aps;
      aps.push_back (make_available_from_manifest ("", "-"));

      const package_name& n (aps.front ().first->id.name);
      const version& v (aps.front ().first->version);

      system_package_manager_fedora m (move (osr),
                                       host_triplet,
                                       ""      /* arch */,
                                       nullopt /* progress */,
                                       nullptr /* options */);

      package_status s (m.map_package (n, v, aps));

      cout <<                              "version: "     << s.system_version << '\n'
           <<                              "main: "        << s.main           << '\n';
      if (!s.devel.empty ())       cout << "devel: "       << s.devel          << '\n';
      if (!s.static_.empty ())     cout << "static: "      << s.static_        << '\n';
      if (!s.doc.empty ())         cout << "doc: "         << s.doc            << '\n';
      if (!s.debuginfo.empty ())   cout << "debuginfo: "   << s.debuginfo      << '\n';
      if (!s.debugsource.empty ()) cout << "debugsource: " << s.debugsource    << '\n';
      if (!s.common.empty ())      cout << "common: "      << s.common         << '\n';
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
      system_package_manager_fedora::simulation s;

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

          i->second.push_back (make_available_from_manifest (n, f));
        }
        else if (
          map<strings, path>* infos =
          k == "dnf-list"           ? &s.dnf_list_ :
          k == "dnf-list-fetched"   ? &s.dnf_list_fetched_   :
          k == "dnf-list-installed" ? &s.dnf_list_installed_ :
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

          infos->emplace (move (ns), path (move (f)));
        }
        else if (map<package, path>* req =
                 k == "dnf-repoquery-requires"         ? &s.dnf_repoquery_requires_         :
                 k == "dnf-repoquery-requires-fetched" ? &s.dnf_repoquery_requires_fetched_ :
                 nullptr)
        {
          size_t q (l.rfind (' ')); assert (q != string::npos);
          string n (l, p + 2, q - p - 2); trim (n);
          string f (l, q + 1); trim (f);

          q = n.rfind (' '); assert (q != string::npos);
          bool i (to_bool (string (n, q + 1)));
          n.resize (q);

          q = n.rfind (' '); assert (q != string::npos);
          string a (n, q + 1);
          n.resize (q);

          q = n.find (' '); assert (q != string::npos);

          package pkg {string (n, 0, q), string (n, q + 1), move (a), i};

          if (f == "!")
            f.clear ();

          req->emplace (move (pkg), path (move (f)));
        }
        else if (k == "dnf-makecache-fail")
        {
          s.dnf_makecache_fail_ = true;
        }
        else if (k == "dnf-install-fail")
        {
          s.dnf_install_fail_ = true;
        }
        else if (k == "dnf-mark-install-fail")
        {
          s.dnf_mark_install_fail_ = true;
        }
        else
          fail << "unknown keyword '" << k << "' in simulation description";
      }

      // Fallback to stubs and sort in the version descending order.
      //
      for (pair<const string, available_packages>& p: aps)
      {
        if (p.second.empty ())
          p.second.push_back (make_available_stub (p.first));

        sort_available (p.second);
      }

      system_package_manager_fedora m (move (osr),
                                       host_triplet,
                                       ""      /* arch */,
                                       nullopt /* progress */,
                                       nullopt /* fetch_timeout */,
                                       install,
                                       fetch,
                                       false   /* yes */,
                                       "sudo");
      m.simulate_ = &s;

      // Query each package.
      //
      for (const string& n: qps)
      {
        package_name pn (n);

        const system_package_status* s (*m.status (pn, &aps[n]));

        assert (*m.status (pn, nullptr) == s); // Test caching.

        if (s == nullptr)
          fail << "no installed " << (install ? "or available " : "")
               << "system package for " << pn;

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

        m.install (ips);
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
