// file      : bpkg/rep-list.cxx -*- C++ -*-
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
  // [(complement|prerequisite) ]<name> <location>[ (<fragment>)]
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

    auto print_repo = [&o, &indent, &chain] (
      const shared_ptr<repository>& r,
      const char* role,
      const repository::fragment_type& fr)
    {
      cout << indent << role << ' ' << r->name << ' ' << r->location;

      if (!fr.friendly_name.empty ())
        cout << " ("  << fr.friendly_name << ")";

      cout << endl;

      print_dependencies (o, r, indent, chain);
    };

    for (const repository::fragment_type& rfr: r->fragments)
    {
      shared_ptr<repository_fragment> fr (rfr.fragment.load ());

      if (o.complements ())
      {
        for (const lazy_weak_ptr<repository>& rp: fr->complements)
        {
          // Skip the root complement (see rep_fetch() for details).
          //
          if (rp.object_id () == "")
            continue;

          print_repo (rp.load (), "complement", rfr);
        }
      }

      if (o.prerequisites ())
      {
        for (const lazy_weak_ptr<repository>& rp: fr->prerequisites)
          print_repo (rp.load (), "prerequisite", rfr);
      }
    }

    indent.resize (indent.size () - 2);
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

    database db (c, trace, false /* pre_attach */);
    transaction t (db);
    session s; // Repository dependencies can have cycles.

    shared_ptr<repository_fragment> root (db.load<repository_fragment> (""));

    for (const lazy_weak_ptr<repository>& rp: root->complements)
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
