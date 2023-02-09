// file      : bpkg/system-package-manager.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager.hxx>

#include <iostream>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#undef NDEBUG
#include <cassert>

#include <bpkg/system-package-manager.test.hxx>

using namespace std;

namespace bpkg
{
  // Usage: args[0] <command> ...
  //
  // Where <command> is one of:
  //
  //   system-package-names <name-id> <ver-id> [<like-id>...] -- [--non-native] <pkg> <file>...
  //
  //     Where <pkg> is a package name, <file> is a package manifest file.
  //
  //   system-package-version <name-id> <ver-id> [<like-id>...] -- <pkg> <file>
  //
  //     Where <pkg> is a package name, <file> is a package manifest file.
  //
  //   downstream-package-version <name-id> <ver-id> [<like-id>...] -- <ver> <pkg> <file>...
  //
  //     Where <ver> is a system version to translate, <pkg> is a package
  //     name, and <file> is a package manifest file.
  //
  int
  main (int argc, char* argv[])
  try
  {
    assert (argc >= 2); // <command>

    int argi (1);
    string cmd (argv[argi++]);

    os_release osr;
    if (cmd == "system-package-names" ||
        cmd == "system-package-version" ||
        cmd == "downstream-package-version")
    {
      assert (argc >= 4); // <name-id> <ver-id>

      osr.name_id = argv[argi++];
      osr.version_id = argv[argi++];

      for (; argi != argc; ++argi)
      {
        string a (argv[argi]);

        if (a == "--")
          break;

        osr.like_ids.push_back (move (a));
      }
    }

    if (cmd == "system-package-names")
    {
      assert (argi != argc); // --
      string a (argv[argi++]);
      assert (a == "--");

      assert (argi != argc);
      bool native (true);
      if ((a = argv[argi]) == "--non-native")
      {
        native = false;
        argi++;
      }

      assert (argi != argc); // <pkg>
      string pn (argv[argi++]);

      assert (argi != argc); // <file>
      available_packages aps;
      for (; argi != argc; ++argi)
        aps.push_back (make_available_from_manifest (pn, argv[argi]));
      sort_available (aps);

      strings ns (
        system_package_manager::system_package_names (
          aps, osr.name_id, osr.version_id, osr.like_ids, native));

      for (const string& n: ns)
        cout << n << '\n';
    }
    else if (cmd == "system-package-version")
    {
      assert (argi != argc); // --
      string a (argv[argi++]);
      assert (a == "--");

      assert (argi != argc); // <pkg>
      string pn (argv[argi++]);

      assert (argi != argc); // <file>
      pair<shared_ptr<available_package>,
           lazy_shared_ptr<repository_fragment>> apf (
             make_available_from_manifest (pn, argv[argi++]));

      assert (argi == argc); // No trailing junk.

      if (optional<string> v =
          system_package_manager::system_package_version (
            apf.first, apf.second, osr.name_id, osr.version_id, osr.like_ids))
      {
        cout << *v << '\n';
      }
    }
    else if (cmd == "downstream-package-version")
    {
      assert (argi != argc); // --
      string a (argv[argi++]);
      assert (a == "--");

      assert (argi != argc); // <ver>
      string sv (argv[argi++]);

      assert (argi != argc); // <pkg>
      string pn (argv[argi++]);

      assert (argi != argc); // <file>
      available_packages aps;
      for (; argi != argc; ++argi)
        aps.push_back (make_available_from_manifest (pn, argv[argi]));
      sort_available (aps);

      optional<version> v (
        system_package_manager::downstream_package_version (
          sv, aps, osr.name_id, osr.version_id, osr.like_ids));

      if (v)
        cout << *v << '\n';
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
