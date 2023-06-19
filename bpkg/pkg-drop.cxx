// file      : bpkg/pkg-drop.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-drop.hxx>

#include <map>
#include <list>
#include <iostream>   // cout

#include <libbutl/path-pattern.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/satisfaction.hxx>
#include <bpkg/manifest-utility.hxx>

#include <bpkg/common-options.hxx>

#include <bpkg/pkg-purge.hxx>
#include <bpkg/pkg-disfigure.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  enum class drop_reason
  {
    user,        // User selection.
    dependent,   // Dependent of a user or another dependent.
    prerequisite // Prerequisite of a user, dependent, or another prerequisite.
  };

  struct drop_package
  {
    database& db;
    shared_ptr<selected_package> package;
    drop_reason reason;
  };

  // List of packages that are dependent on the user selection.
  //
  struct dependent_name
  {
    database& db;
    package_name name;
    database& prq_db;
    package_name prq_name; // Prerequisite package name.
  };
  using dependent_names = vector<dependent_name>;

  // A "dependency-ordered" list of packages and their prerequisites. That is,
  // every package on the list only possibly depending on the ones after it.
  // In a nutshell, the usage is as follows: we first add the packages
  // specified by the user (the "user selection"). We then collect all the
  // dependent packages of the user selection, if any. These will either have
  // to be dropped as well or we cannot continue and need to either issue
  // diagnostics and fail or exit with the specified (via --dependent-exit)
  // code. If the user gave the go ahead to drop the dependents, then, for our
  // purposes, this list of dependents can from now own be treated as if it
  // was a part of the user selection. The next step is to collect all the
  // non-held prerequisites of the user selection with the goal of figuring
  // out which ones will no longer be needed and offering to drop them as
  // well. This part is a bit tricky and has to be done in three steps: We
  // first collect all the prerequisites that we could possibly be dropping.
  // We then order all the packages. And, finally, we filter out prerequisites
  // that we cannot drop. See the comment to the call to
  // collect_prerequisites() for details on why it has to be done this way.
  //
  struct drop_packages: list<reference_wrapper<drop_package>>
  {
    // Collect a package to be dropped, by default, as a user selection.
    //
    bool
    collect (database& db,
             shared_ptr<selected_package> p,
             drop_reason r = drop_reason::user)
    {
      package_name n (p->name); // Because of move(p) below.
      return map_.emplace (package_key {db, move (n)},
                           data_type {end (), {db, move (p), r}}).second;
    }

    // Collect all the dependents of the user selection returning the list
    // of their names. Dependents of dependents are collected recursively.
    //
    dependent_names
    collect_dependents ()
    {
      dependent_names dns;

      for (const auto& pr: map_)
      {
        const drop_package& dp (pr.second.package);

        // Unconfigured package cannot have any dependents.
        //
        if (dp.reason != drop_reason::dependent &&
            dp.package->state == package_state::configured)
          collect_dependents (pr.first.db, dp.package, dns);
      }

      return dns;
    }

    void
    collect_dependents (database& db,
                        const shared_ptr<selected_package>& p,
                        dependent_names& dns)
    {
      for (database& ddb: db.dependent_configs ())
      {
        for (auto& pd: query_dependents_cache (ddb, p->name, db))
        {
          const package_name& dn (pd.name);

          if (map_.find (ddb, dn) == map_.end ())
          {
            shared_ptr<selected_package> dp (ddb.load<selected_package> (dn));
            dns.push_back (dependent_name {ddb, dn, db, p->name});
            collect (ddb, dp, drop_reason::dependent);
            collect_dependents (ddb, dp, dns);
          }
        }
      }
    }

    // Collect prerequisites of the user selection and its dependents,
    // returning true if any were collected. Prerequisites of prerequisites
    // are collected recursively.
    //
    bool
    collect_prerequisites ()
    {
      bool r (false);

      for (const auto& pr: map_)
      {
        const drop_package& dp (pr.second.package);

        // Unconfigured package cannot have any prerequisites.
        //
        if ((dp.reason == drop_reason::user ||
             dp.reason == drop_reason::dependent) &&
            dp.package->state == package_state::configured)
          r = collect_prerequisites (dp.package) || r;
      }

      return r;
    }

    bool
    collect_prerequisites (const shared_ptr<selected_package>& p)
    {
      bool r (false);

      for (const auto& pair: p->prerequisites)
      {
        const lazy_shared_ptr<selected_package>& lpp (pair.first);
        database& pdb (lpp.database ());

        if (map_.find (pdb, lpp.object_id ()) == map_.end ())
        {
          shared_ptr<selected_package> pp (lpp.load ());

          if (!pp->hold_package) // Prune held packages.
          {
            collect (pdb, pp, drop_reason::prerequisite);
            collect_prerequisites (pp);
            r = true;
          }
        }
      }

      return r;
    }

    // Order the previously-collected package with the specified name
    // returning its positions.
    //
    iterator
    order (database& db, const package_name& name)
    {
      // Every package that we order should have already been collected.
      //
      auto mi (map_.find (db, name));
      assert (mi != map_.end ());

      // If this package is already in the list, then that would also
      // mean all its prerequisites are in the list and we can just
      // return its position.
      //
      iterator& pos (mi->second.position);
      if (pos != end ())
        return pos;

      // Order all the prerequisites of this package and compute the
      // position of its "earliest" prerequisite -- this is where it
      // will be inserted.
      //
      drop_package& dp (mi->second.package);
      const shared_ptr<selected_package>& p (dp.package);

      // Unless this package needs something to be before it, add it to
      // the end of the list.
      //
      iterator i (end ());

      // Figure out if j is before i, in which case set i to j. The goal
      // here is to find the position of our "earliest" prerequisite.
      //
      auto update = [this, &i] (iterator j)
      {
        for (iterator k (j); i != j && k != end ();)
          if (++k == i)
            i = j;
      };

      // Only configured packages have prerequisites.
      //
      if (p->state == package_state::configured)
      {
        for (const auto& pair: p->prerequisites)
        {
          database& pdb (pair.first.database ());
          const package_name& pn (pair.first.object_id ());

          // The prerequisites may not necessarily be in the map (e.g.,
          // a held package that we prunned).
          //
          if (map_.find (pdb, pn) != map_.end ())
            update (order (pdb, pn));
        }
      }

      return pos = insert (i, dp);
    }

    // Remove prerequisite packages that we cannot possibly drop, returning
    // true if any remain.
    //
    bool
    filter_prerequisites ()
    {
      bool r (false);

      // Iterate from "more" to "less"-dependent.
      //
      for (auto i (begin ()); i != end (); )
      {
        const drop_package& dp (*i);

        if (dp.reason == drop_reason::prerequisite)
        {
          const shared_ptr<selected_package>& p (dp.package);
          database& db (dp.db);

          bool keep (true);

          // Get our dependents (which, BTW, could only have been before us
          // on the list). If they are all in the map, then we can be dropped.
          //
          for (database& ddb: db.dependent_configs ())
          {
            for (auto& pd: query_dependents (ddb, p->name, db))
            {
              if (map_.find (ddb, pd.name) == map_.end ())
              {
                keep = false;
                break;
              }
            }

            if (!keep)
              break;
          }

          if (!keep)
          {
            i = erase (i);
            map_.erase (package_key {db, p->name});
            continue;
          }

          r = true;
        }

        ++i;
      }

      return r;
    }

  private:
    struct data_type
    {
      iterator position;    // Note: can be end(), see collect().
      drop_package package;
    };

    class package_map: public map<package_key, data_type>
    {
    public:
      using base_type = map<package_key, data_type>;

      iterator
      find (database& db, const package_name& pn)
      {
        return base_type::find (package_key {db, pn});
      }
    };
    package_map map_;
  };

  // Drop ordered list of packages.
  //
  static int
  pkg_drop (const pkg_drop_options& o,
            const drop_packages& pkgs,
            bool drop_prq,
            bool need_prompt)
  {
    // Print what we are going to do, then ask for the user's confirmation.
    //
    if (o.print_only ()     ||
        o.plan_specified () ||
        !(o.yes () || o.no () || !need_prompt))
    {
      bool first (true); // First entry in the plan.

      for (const drop_package& dp: pkgs)
      {
        // Skip prerequisites if we weren't instructed to drop them.
        //
        if (dp.reason == drop_reason::prerequisite && !drop_prq)
          continue;

        const shared_ptr<selected_package>& p (dp.package);

        if (first)
        {
          // If the plan header is not empty, now is the time to print it.
          //
          if (!o.plan ().empty ())
          {
            if (o.print_only ())
              cout << o.plan () << endl;
            else
              text << o.plan ();
          }

          first = false;
        }

        if (o.print_only ())
          cout << "drop " << p->name << dp.db << endl;
        else if (verb)
          // Print indented for better visual separation.
          //
          text << "  drop " << p->name << dp.db;
      }

      if (o.print_only ())
        return 0;
    }

    // Ask the user if we should continue.
    //
    if (o.no () ||
        !(o.yes () || !need_prompt || yn_prompt ("continue? [Y/n]", 'y')))
      return 1;

    bool result (verb && !o.no_result ());
    bool progress (!result &&
                   ((verb == 1 && !o.no_progress () && stderr_term) ||
                    o.progress ()));

    size_t prog_i, prog_n, prog_percent;

    // All that's left to do is first disfigure configured packages and
    // then purge all of them. We do both left to right (i.e., from more
    // dependent to less dependent). For disfigure this order is required.
    // For purge, it will be the order closest to the one specified by the
    // user.
    //
    // Note: similar code in pkg-build.
    //
    auto disfigure_pred = [drop_prq] (const drop_package& dp)
    {
      // Skip prerequisites if we weren't instructed to drop them.
      //
      if (dp.reason == drop_reason::prerequisite && !drop_prq)
        return false;

      if (dp.package->state != package_state::configured)
        return false;

      return true;
    };

    if (progress)
    {
      prog_i = 0;
      prog_n = static_cast<size_t> (count_if (pkgs.begin (), pkgs.end (),
                                              disfigure_pred));
      prog_percent = 100;
    }

    for (const drop_package& dp: pkgs)
    {
      if (!disfigure_pred (dp))
        continue;

      database& db (dp.db);
      const shared_ptr<selected_package>& p (dp.package);

      // Each package is disfigured in its own transaction, so that we always
      // leave the configuration in a valid state.
      //
      transaction t (db);

      // Commits the transaction.
      //
      pkg_disfigure (o, db, t,
                     p,
                     true /* clean */,
                     true /* disfigure */,
                     false /* simulate */);

      assert (p->state == package_state::unpacked ||
              p->state == package_state::transient);

      if (result || progress)
      {
        const char* what (p->state == package_state::transient
                          ? "purged"
                          : "disfigured");
        if (result)
          text << what << ' ' << p->name << db;
        else if (progress)
        {
          size_t p ((++prog_i * 100) / prog_n);

          if (prog_percent != p)
          {
            prog_percent = p;

            diag_progress_lock pl;
            diag_progress  = ' ';
            diag_progress += to_string (p);
            diag_progress += "% of packages ";
            diag_progress += what;
          }
        }
      }
    }

    // Clear the progress if shown.
    //
    if (progress)
    {
      diag_progress_lock pl;
      diag_progress.clear ();
    }

    if (o.disfigure_only ())
      return 0;

    // Purge.
    //
    for (const drop_package& dp: pkgs)
    {
      // Skip prerequisites if we weren't instructed to drop them.
      //
      if (dp.reason == drop_reason::prerequisite && !drop_prq)
        continue;

      const shared_ptr<selected_package>& p (dp.package);

      if (p->state == package_state::transient) // Fully purged by disfigure.
        continue;

      assert (p->state == package_state::fetched ||
              p->state == package_state::unpacked);

      database& db (dp.db);

      transaction t (db);

      // Commits the transaction, p is now transient.
      //
      pkg_purge (db, t, p, false /* simulate */);

      if (result)
        text << "purged " << p->name << db;
    }

    return 0;
  }

  int
  pkg_drop (const pkg_drop_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_drop");

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    {
      diag_record dr;

      if (o.yes () && o.no ())
      {
        dr << fail << "both --yes|-y and --no|-n specified";
      }
      else if (o.drop_dependent () && o.keep_dependent ())
      {
        dr << fail << "both --drop-dependent and --keep-dependent|-K "
                   << "specified";
      }
      else if (o.drop_dependent () && o.dependent_exit_specified ())
      {
        dr << fail << "both --drop-dependent and --dependent-exit "
                   << "specified";
      }
      else if (o.keep_dependent () && o.dependent_exit_specified ())
      {
        dr << fail << "both --keep-dependent|-K and --dependent-exit "
                   << "specified";
      }
      else if (o.all ())
      {
        if (o.all_pattern_specified ())
          dr << fail << "both --all|-a and --all-pattern specified";

        if (args.more ())
          dr << fail << "both --all|-a and package argument specified";
      }
      else if (o.all_pattern_specified ())
      {
        if (args.more ())
          dr << fail << "both --all-pattern and package argument specified";
      }
      else if (!args.more ())
      {
        dr << fail << "package name argument expected";
      }

      if (!dr.empty ())
        dr << info << "run 'bpkg help pkg-drop' for more information";
    }


    database db (c, trace, true /* pre_attach */);

    // Note that the session spans all our transactions. The idea here is
    // that drop_package objects in the drop_packages list below will be
    // cached in this session. When subsequent transactions modify any of
    // these objects, they will modify the cached instance, which means
    // our list will always "see" their updated state.
    //
    session s;

    // Assemble the list of packages we will need to drop.
    //
    drop_packages pkgs;
    bool drop_prq (false);

    // We need the plan and to ask for the user's confirmation only if there
    // are additional packages (such as dependents or prerequisites of the
    // explicitly listed packages) to be dropped. But if the user explicitly
    // requested it with --plan, then we print it as long as it is not empty.
    //
    bool need_prompt (false);
    {
      transaction t (db);

      // The first step is to load and collect all the packages specified
      // by the user.
      //
      vector<package_name> names;

      auto add = [&names, &pkgs, &db] (shared_ptr<selected_package>&& p)
      {
        package_name n (p->name);

        if (p->state == package_state::broken)
          fail << "unable to drop broken package " << n <<
            info << "use 'pkg-purge --force' to remove";

        if (pkgs.collect (db, move (p)))
          names.push_back (move (n));
      };

      if (o.all () || o.all_pattern_specified ())
      {
        using query = query<selected_package>;

        for (shared_ptr<selected_package> p:
               pointer_result (
                 db.query<selected_package> (query::hold_package)))
        {
          l4 ([&]{trace << *p;});

          if (o.all_pattern_specified ())
          {
            for (const string& pat: o.all_pattern ())
            {
              if (path_match (p->name.string (), pat))
              {
                add (move (p));
                break;
              }
            }
          }
          else // --all
            add (move (p));
        }

        if (names.empty ())
          info << "nothing to drop";
      }
      else
      {
        while (args.more ())
        {
          package_name n (parse_package_name (args.next (),
                                              false /* allow_version */));

          l4 ([&]{trace << "package " << n;});

          shared_ptr<selected_package> p (db.find<selected_package> (n));

          if (p == nullptr)
            fail << "package " << n << " does not exist in configuration " << c;

          add (move (p));
        }
      }

      // The next step is to see if there are any dependents that are not
      // already on the list. We will either have to drop those as well or
      // issue diagnostics and fail or silently indicate that with an exit
      // code.
      //
      dependent_names dnames (pkgs.collect_dependents ());
      if (!dnames.empty () && !o.drop_dependent ())
      {
        if (o.dependent_exit_specified ())
        {
          t.commit ();
          return o.dependent_exit ();
        }

        {
          diag_record dr;

          if (o.keep_dependent ())
            dr << fail;
          else
            dr << text;

          dr << "following dependent packages will have to be dropped "
             << "as well:";

          for (const dependent_name& dn: dnames)
            dr << text << dn.name << dn.db << " (requires " << dn.prq_name
               << dn.prq_db << ")";
        }

        if (o.yes ())
          fail << "refusing to drop dependent packages with just --yes" <<
            info << "specify --drop-dependent to confirm";

        if (o.no () || !yn_prompt ("drop dependent packages? [y/N]", 'n'))
          return 1;

        need_prompt = true;
      }

      // Collect all the prerequisites that are not held. These will be
      // the candidates to drop as well. Note that we cannot make the
      // final decision who we can drop until we have the complete and
      // ordered list of all the packages that we could potentially be
      // dropping. The ordered part is important: we will have to decide
      // about the "more dependent" prerequisite before we can decide
      // about the "less dependent" one since the former could be depending
      // on the latter and, if that's the case and "more" cannot be dropped,
      // then neither can "less".
      //
      pkgs.collect_prerequisites ();

      // Now that we have collected all the packages we could possibly be
      // dropping, arrange them in the "dependency order", that is, with
      // every package on the list only possibly depending on the ones
      // after it.
      //
      // First order the user selection so that we stay as close to the
      // order specified by the user as possible. Then order the dependent
      // packages. Since each of them depends on one or more packages from
      // the user selection, it will be inserted before the first package
      // on which it depends.
      //
      for (const package_name& n: names)
        pkgs.order (db, n);

      for (const dependent_name& dn: dnames)
        pkgs.order (dn.db, dn.name);

      // Filter out prerequisites that we cannot possibly drop (e.g., they
      // have dependents other than the ones we are dropping). If there are
      // some that we can drop, ask the user for confirmation.
      //
      if (pkgs.filter_prerequisites () &&
          !o.keep_unused ()            &&
          !(drop_prq = o.yes ()) && !o.no ())
      {
        {
          diag_record dr (text);

          dr << "following dependencies were automatically built but will "
             << "no longer be used:";

          for (const drop_package& dp: pkgs)
          {
            if (dp.reason == drop_reason::prerequisite)
              dr << text << (dp.package->system () ? "sys:" : "")
                 << dp.package->name << dp.db;
          }
        }

        drop_prq = yn_prompt ("drop unused packages? [Y/n]", 'y');

        if (drop_prq)
          need_prompt = true;
      }

      t.commit ();
    }

    return pkg_drop (o, pkgs, drop_prq, need_prompt);
  }
}
