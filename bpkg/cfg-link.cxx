// file      : bpkg/cfg-link.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/cfg-link.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;

namespace bpkg
{
  shared_ptr<configuration>
  cfg_link (database& db,
            const dir_path& ld,
            bool rel,
            optional<string> name,
            bool sys_rep)
  {
    tracer trace ("cfg_link");

    bool name_specified (name);
    const dir_path& cd (db.config); // Note: absolute and normalized.

    // Load the self-link object from the database of the configuration being
    // linked to obtain its name, type, and uuid.
    //
    database& ldb (db.attach (ld, sys_rep));

    string type;
    uuid uid;
    {
      shared_ptr<configuration> cf (ldb.load<configuration> (0));

      type = move (cf->type);
      uid  = cf->uuid;

      if (!name)
        name = move (cf->name);
    }

    if (db.uuid == uid)
      fail << "linking configuration " << ld << " with itself" <<
        info << "uuid: " << uid;

    if (name && name == db.name)
      fail << "linking configuration " << ld << " using current "
           << "configuration name '" << *name << "'" <<
        info << "consider specifying alternative name with --name";

    // Verify that the name and path of the configuration being linked do not
    // clash with already linked configurations. Fail if configurations with
    // this uuid is already linked unless the link is implicit, in which case
    // make it explicit and update its name and path.
    //
    // Note that when we make an implicit link explicit, we start treating it
    // as an implicit and explicit simultaneously. So, for example, for cfg1
    // the link cfg2 is explicit and the link cfg3 is both explicit and
    // implicit:
    //
    // cfg2 <- cfg1 <-> cfg3
    //
    // Similar, if we link cfg1 with cfg2, the explicit link cfg2 in cfg1 also
    // becomes both explicit and implicit, not being amended directly.
    //
    shared_ptr<configuration> lcf;

    using query = query<configuration>;

    for (shared_ptr<configuration> lc:
           pointer_result (db.query<configuration> (query::id != 0)))
    {
      if (uid == lc->uuid)
      {
        if (lc->expl)
          fail << "configuration with uuid " << uid << " is already linked "
               << "as " << lc->path;

        // Verify the existing implicit link integrity and cache it to update
        // later, when the name/path clash check is complete.
        //
        db.verify_link (*lc, ldb);

        lcf = move (lc);
        continue;
      }

      if (ld == lc->effective_path (cd))
        fail << "configuration with path " << ld << " is already linked";

      // If the name clashes, then fail if it was specified by the user and
      // issue a warning and link the configuration as unnamed otherwise.
      //
      if (name && name == lc->name)
      {
        diag_record dr (name_specified ? error : warn);
        dr << "configuration with name " << *name << " is already linked as "
           << lc->path;

        if (name_specified)
        {
          dr << info << "consider specifying alternative name with --name"
             << endf;
        }
        else
        {
          dr << ", linking as unnamed";
          name = nullopt;
        }
      }
    }

    // If requested, rebase the first path relative to the second or return it
    // as is otherwise. Fail if the rebase is not possible (e.g., paths are on
    // different drives on Windows).
    //
    auto rebase = [rel] (const dir_path& x, const dir_path& y) -> dir_path
    {
      try
      {
        return rel ? x.relative (y) : x;
      }
      catch (const invalid_path&)
      {
        fail << "unable to rebase " << x << " relative to " << y <<
          info << "specify absolute configuration directory path to save it "
               << "as absolute" << endf;
      }
    };

    // If the implicit link already exists, then make it explicit and update
    // its name and path. Otherwise, create a new link.
    //
    // Note that in the former case the current configuration must already be
    // explicitly linked with the configuration being linked. We verify that
    // and the link integrity.
    //
    if (lcf != nullptr)
    {
      // Verify the backlink integrity.
      //
      shared_ptr<configuration> cf (
        ldb.query_one<configuration> (query::uuid == db.uuid.string ()));

      // Note: both sides of the link cannot be implicit.
      //
      if (cf == nullptr || !cf->expl)
        fail << "configuration " << ld << " is already implicitly linked but "
             << "current configuration " << cd << " is not explicitly linked "
             << "with it";

      ldb.verify_link (*cf, db);

      // Finally, turn the implicit link into explicit.
      //
      // Note: reuse id.
      //
      lcf->expl = true;
      lcf->name = move (name);
      lcf->path = rebase (ld, cd); // Note: can't clash (see above).

      db.update (lcf);
    }
    else
    {
      // If the directory path of the configuration being linked is relative
      // or the --relative option is specified, then rebase it relative to the
      // current configuration directory path.
      //
      lcf = make_shared<configuration> (uid,
                                        move (name),
                                        move (type),
                                        rebase (ld, cd),
                                        true /* explicit */);

      db.persist (lcf);

      // Now implicitly link ourselves with the just linked configuration.
      // Note that we link ourselves as unnamed.
      //
      shared_ptr<configuration> ccf (db.load<configuration> (0));

      // What if we find the current configuration to already be implicitly
      // linked? The potential scenario could be, that the current
      // configuration was recreated from scratch, previously being implicitly
      // linked with the configuration we currently link. It feels like in
      // this case we would rather overwrite the existing dead implicit link
      // than just fail. Let's also warn for good measure.
      //
      shared_ptr<configuration> cf;

      for (shared_ptr<configuration> lc:
             pointer_result (ldb.query<configuration> (query::id != 0)))
      {
        if (cd == lc->make_effective_path (ld))
        {
          if (lc->expl)
            fail << "current configuration " << cd << " is already linked "
                 << "with " << ld;

          warn << "current configuration " << cd << " is already implicitly "
               << "linked with " << ld;

          cf = move (lc);
          continue;
        }

        if (ccf->uuid == lc->uuid)
          fail << "current configuration " << ccf->uuid << " is already "
               << "linked with " << ld;
      }

      // It feels natural to persist explicitly and implicitly linked
      // configuration paths both either relative or absolute.
      //
      if (cf != nullptr)
      {
        // The dead implicit link case.
        //
        // Note: reuse id.
        //
        cf->uuid = ccf->uuid;
        cf->type = move (ccf->type);
        cf->path = rebase (cd, ld);

        ldb.update (cf);
      }
      else
      {
        ccf = make_shared<configuration> (ccf->uuid,
                                          nullopt /* name */,
                                          move (ccf->type),
                                          rebase (cd, ld),
                                          false /* explicit */);

        ldb.persist (ccf);
      }
    }

    // If explicit links of the current database are pre-attached, then also
    // pre-attach explicit links of the newly linked database.
    //
    linked_configs& lcs (db.explicit_links ());

    if (!lcs.empty ())
    {
      lcs.push_back (linked_config {*lcf->id, lcf->name, ldb});
      ldb.attach_explicit (sys_rep);
    }

    // If the implicit links of the linked database are already cached, then
    // also cache the current database, unless it is already there (see above
    // for the dead link case).
    //
    linked_databases& lds (ldb.implicit_links (false /* attach */));

    if (!lds.empty () && find (lds.begin (), lds.end (), db) == lds.end ())
      lds.push_back (db);

    return lcf;
  }

  int
  cfg_link (const cfg_link_options& o, cli::scanner& args)
  try
  {
    tracer trace ("cfg_link");

    dir_path c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    if (o.name_specified ())
      validate_configuration_name (o.name (), "--name option value");

    if (!args.more ())
      fail << "configuration directory argument expected" <<
        info << "run 'bpkg help cfg-link' for more information";

    dir_path ld (args.next ());
    if (ld.empty ())
      throw invalid_path (ld.string ());

    l4 ([&]{trace << "link configuration: " << ld;});

    bool rel (ld.relative () || o.relative ());
    normalize (ld, "specified linked configuration");

    database db (c, trace, false /* pre_attach */, false /* sys_rep */, {ld});
    transaction t (db);

    shared_ptr<configuration> lc (
      cfg_link (db,
                ld,
                rel,
                o.name_specified () ? o.name () : optional<string> ()));

    t.commit ();

    if (verb && !o.no_result ())
    {
      diag_record dr (text);

      dr << "linked with configuration " << ld << '\n'
         << "  uuid: " << lc->uuid << '\n'
         << "  type: " << lc->type << '\n';

      if (lc->name)
        dr << "  name: " << *lc->name << '\n';

      dr << "  id:   " << *lc->id;
    }

    return 0;
  }
  catch (const invalid_path& e)
  {
    fail << "invalid path: '" << e.path << "'" << endf;
  }
}
