// file      : bpkg/rep-list.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-list.hxx>

#include <set>
#include <iostream>  // cout

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;

namespace bpkg
{
  // Print the repository dependencies, recursively.
  //
  // Each line has the following form:
  //
  // [(complement|prerequisite) ]<name> <location>
  //
  // and is indented with 2 additional spaces for each recursion level.
  //
  // Note that we can end up with a repository dependency cycle via
  // prerequisites. Thus we need to make sure that the repository is not in
  // the dependency chain yet.
  //
  using repositories = set<reference_wrapper<const shared_ptr<repository>>,
                           compare_reference_target>;

  static void
  print_dependencies (const rep_list_options& o,
                      const shared_ptr<repository>& r,
                      string& indent,
                      repositories& chain)
  {
    assert (!r->name.empty ()); // Can't be the root repository.

    if (!chain.insert (r).second) // Is already in the chain.
      return;

    indent += "  ";

    if (o.complements ())
    {
      for (const lazy_shared_ptr<repository>& rp: r->complements)
      {
        // Skip the root complement (see rep_fetch() for details).
        //
        if (rp.object_id () == "")
          continue;

        shared_ptr<repository> r (rp.load ());

        cout << indent << "complement "
             << r->location.canonical_name () << " " << r->location << endl;

        print_dependencies (o, r, indent, chain);
      }
    }

    if (o.prerequisites ())
    {
      for (const lazy_weak_ptr<repository>& rp: r->prerequisites)
      {
        shared_ptr<repository> r (rp.load ());

        cout << indent << "prerequisite "
             << r->location.canonical_name () << " " << r->location << endl;

        print_dependencies (o, r, indent, chain);
      }
    }

    indent.pop_back ();
    indent.pop_back ();

    chain.erase (r);
  }

  static inline void
  print_dependencies (const rep_list_options& o,
                      const shared_ptr<repository>& r)
  {
    string indent;
    repositories chain;
    print_dependencies (o, r, indent, chain);
  }

  int
  rep_list (const rep_list_options& o, cli::scanner& args)
  {
    tracer trace ("rep_list");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    if (args.more ())
      fail << "unexpected argument '" << args.next () << "'" <<
        info << "run 'bpkg help rep-list' for more information";

    database db (open (c, trace));
    transaction t (db.begin ());
    session s; // Repository dependencies can have cycles.

    shared_ptr<repository> root (db.load<repository> (""));

    for (const lazy_shared_ptr<repository>& rp: root->complements)
    {
      shared_ptr<repository> r (rp.load ());
      cout << r->location.canonical_name () << " " << r->location << endl;

      if (o.complements () || o.prerequisites ())
        print_dependencies (o, r);
    }

    t.commit ();

    return 0;
  }
}
