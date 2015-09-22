// file      : bpkg/rep-add.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-add>

#include <stdexcept> // invalid_argument

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
  rep_add (const rep_add_options& o, cli::scanner& args)
  {
    tracer trace ("rep_add");

    dir_path c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "repository location argument expected" <<
        info << "run 'bpkg help rep-add' for more information";

    // Figure out the repository location.
    //
    const char* arg (args.next ());
    repository_location rl;
    try
    {
      rl = repository_location (arg, repository_location ());

      if (rl.relative ()) // Throws if location is empty.
        rl = repository_location (
          dir_path (arg).complete ().normalize ().string ());
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid repository location '" << arg << "': " << e.what ();
    }

    const string& rn (rl.canonical_name ());

    // Create the new repository and add is as a complement to the root.
    //
    database db (open (c, trace));
    transaction t (db.begin ());
    session s; // Repository dependencies can have cycles.

    // It is possible that this repository is already in the database.
    // For example, it might be a prerequisite of one of the already
    // added repository.
    //
    shared_ptr<repository> r (db.find<repository> (rl.canonical_name ()));

    if (r == nullptr)
    {
      r.reset (new repository (rl));
      db.persist (r);
    }

    shared_ptr<repository> root (db.load<repository> (""));

    if (!root->complements.insert (lazy_shared_ptr<repository> (db, r)).second)
    {
      fail << rn << " is already a repository of this configuration";
    }

    db.update (root);
    t.commit ();

    if (verb)
      text << "added " << rn;
  }
}
