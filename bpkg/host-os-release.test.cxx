// file      : bpkg/host-os-release.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/host-os-release.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <iostream>

#undef NDEBUG
#include <cassert>

using namespace std;

namespace bpkg
{
  extern os_release
  host_os_release_linux (path f = {});

  int
  main (int argc, char* argv[])
  {
    assert (argc >= 2); // <host-target-triplet>

    target_triplet host (argv[1]);

    os_release r;
    if (host.class_ == "linux")
    {
      assert (argc == 3); // <host-target-triplet> <file-path>
      r = host_os_release_linux (path (argv[2]));
    }
    else
      assert (false);

    cout << r.name_id << '\n';
    for (auto b (r.like_ids.begin ()), i (b); i != r.like_ids.end (); ++i)
      cout << (i != b ? "|" : "") << *i;
    cout << '\n'
         << r.version_id << '\n'
         << r.variant_id << '\n'
         << r.name << '\n'
         << r.version_codename << '\n'
         << r.variant << '\n';

    return 0;
  }
}

int
main (int argc, char* argv[])
{
  return bpkg::main (argc, argv);
}
