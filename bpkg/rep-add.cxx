// file      : bpkg/rep-add.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/rep-add.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  int
  rep_add (const rep_add_options& o, cli::scanner& args)
  {
    tracer trace ("rep_add");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "repository location argument expected" <<
        info << "run 'bpkg help rep-add' for more information";

    database db (open (c, trace));
    transaction t (db.begin ());
    session s; // Repository dependencies can have cycles.

    shared_ptr<repository> root (db.load<repository> (""));

    while (args.more ())
    {
      repository_location rl (
        parse_location (args.next (),
                        o.type_specified ()
                        ? optional<repository_type> (o.type ())
                        : nullopt));

      const string& rn (rl.canonical_name ());

      // Create the new repository if it is not in the database yet. Otherwise
      // update its location. Add it as a complement to the root repository (if
      // it is not there yet).
      //
      shared_ptr<repository> r (db.find<repository> (rn));

      bool updated (false);

      if (r == nullptr)
      {
        r.reset (new repository (rl));
        db.persist (r);
      }
      else if (r->location.url () != rl.url ())
      {
        r->location = rl;
        db.update (r);

        updated = true;
      }

      bool added (
        root->complements.insert (lazy_shared_ptr<repository> (db, r)).second);

      if (verb)
        text << (added ? "added " : updated ? "updated " : "unchanged ") << rn;
    }

    db.update (root);
    t.commit ();

    return 0;
  }
}
