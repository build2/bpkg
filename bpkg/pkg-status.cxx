// file      : bpkg/pkg-status.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-status>

#include <iostream> // cout

#include <bpkg/types>
#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/utility>
#include <bpkg/database>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  pkg_status (const pkg_status_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_status");

    dir_path c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help pkg-status' for more information";

    string n (args.next ());

    version v;
    if (args.more ())
    {
      const char* s (args.next ());
      try
      {
        v = version (s);
      }
      catch (const invalid_argument& e)
      {
        fail << "invalid package version '" << s << "'";
      }
    }

    database db (open (c));
    transaction t (db.begin ());

    using query = odb::query<package>;
    query q (query::name == n);

    if (!v.empty ())
    {
      q = q && (query::version.epoch == v.epoch () &&
                query::version.revision == v.revision () &&
                query::version.canonical_upstream == v.canonical_upstream ());
    }

    shared_ptr<package> p (db.query_one<package> (q));

    if (p == nullptr)
    {
      // @@ TODO: This is where we search the packages available in
      // the repositories and if found, print its status as 'available'
      // plus a list of versions.
      //
      cout << "unknown";
    }
    else
    {
      cout << p->state;

      // Also print the version of the package unless the user
      // specified it.
      //
      if (v.empty ())
        cout << " " << p->version;
    }

    cout << endl;
  }
}
