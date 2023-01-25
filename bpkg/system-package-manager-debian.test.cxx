// file      : bpkg/system-package-manager-debian.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager-debian.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <iostream>

#undef NDEBUG
#include <cassert>

using namespace std;

namespace bpkg
{
  using package_status = system_package_status_debian;
  using package_policy = package_status::package_policy;

  // Usage: args[0] <command> ...
  //
  // Where <command> is one of:
  //
  // apt-cache-policy <pkg>...          result comes from stdin
  //
  // apt-cache-show <pkg> <ver>         result comes from stdin
  //
  // main-from-dev <dev-pkg> <dev-ver>  depends comes from stdin
  //
  int
  main (int argc, char* argv[])
  try
  {
    assert (argc >= 2); // <command>

    string cmd (argv[1]);

    // @@ TODO: add option to customize.
    //
    os_release osr {"debian", {}, "10", "", "Debian", "", ""};

    system_package_manager_debian::simulation s;
    system_package_manager_debian m (move (osr),
                                     false   /* install */,
                                     false   /* fetch */,
                                     nullopt /* progress */,
                                     false   /* yes */,
                                     "sudo");
    m.simulate_ = &s;

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

      s.apt_cache_policy_.emplace (move (key), path ("-"));

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

      s.apt_cache_show_.emplace (key, path ("-"));

      cout << m.apt_cache_show (key.first, key.second) << "\n";
    }
    else if (cmd == "main-from-dev")
    {
      assert (argc == 4); // <dev-pkg> <dev-ver>

      string n (argv[2]);
      string v (argv[3]);
      string d;
      getline (cin, d, '\0');

      cout << m.main_from_dev (n, v, d) << "\n";
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
