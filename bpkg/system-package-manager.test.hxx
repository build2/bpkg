// file      : bpkg/system-package-manager.test.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_SYSTEM_PACKAGE_MANAGER_TEST_HXX
#define BPKG_SYSTEM_PACKAGE_MANAGER_TEST_HXX

#include <bpkg/system-package-manager.hxx>

#include <algorithm> // sort()

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <libbutl/manifest-parser.hxx>

#include <libbpkg/manifest.hxx>

#include <bpkg/package.hxx>

namespace bpkg
{
  // Parse the manifest as if it comes from a git repository with a single
  // package and make an available package out of it. If the file name is
  // `-` then read fro stdin. If the package name is empty, then take the
  // name from the manifest. Otherwise, assert they match.
  //
  inline
  pair<shared_ptr<available_package>, lazy_shared_ptr<repository_fragment>>
  make_available_from_manifest (const string& pn, const string& f)
  {
    using butl::manifest_parser;
    using butl::manifest_parsing;

    path fp (f);
    path_name fn (fp);

    try
    {
      ifdstream ifds;
      istream& ifs (butl::open_file_or_stdin (fn, ifds));

      manifest_parser mp (ifs, fn.name ? *fn.name : fn.path->string ());

      package_manifest m (mp,
                          false /* ignore_unknown */,
                          true /* complete_values */);

      const string& n (m.name.string ());
      assert (pn.empty () || n == pn);

      m.alt_naming = false;
      m.bootstrap_build = "project = " + n + '\n';

      shared_ptr<available_package> ap (
        make_shared<available_package> (move (m)));

      lazy_shared_ptr<repository_fragment> af (
        make_shared<repository_fragment> (
          repository_location ("https://example.com/" + n,
                               repository_type::git)));

      ap->locations.push_back (package_location {af, current_dir});

      return make_pair (move (ap), move (af));
    }
    catch (const manifest_parsing& e)
    {
      fail (e.name, e.line, e.column) << e.description << endf;
    }
    catch (const io_error& e)
    {
      fail << "unable to read from " << fn << ": " << e << endf;
    }
  }

  // Make an available stub package as if it comes from git repository with
  // a single package.
  //
  inline
  pair<shared_ptr<available_package>, lazy_shared_ptr<repository_fragment>>
  make_available_stub (const string& n)
  {
    shared_ptr<available_package> ap (
      make_shared<available_package> (package_name (n)));

    lazy_shared_ptr<repository_fragment> af (
      make_shared<repository_fragment> (
        repository_location ("https://example.com/" + n,
                             repository_type::git)));

    ap->locations.push_back (package_location {af, current_dir});

    return make_pair (move (ap), move (af));
  }

  // Sort available packages in the version descending order.
  //
  inline void
  sort_available (available_packages& aps)
  {
    using element_type =
      pair<shared_ptr<available_package>, lazy_shared_ptr<repository_fragment>>;

    std::sort (aps.begin (), aps.end (),
               [] (const element_type& x, const element_type& y)
               {
                 return x.first->version > y.first->version;
               });
  }
}

#endif // BPKG_SYSTEM_PACKAGE_MANAGER_TEST_HXX
