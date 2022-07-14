// file      : bpkg/package-configuration.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package-configuration.hxx>

#include <sstream>

#include <bpkg/package-skeleton.hxx>

namespace bpkg
{
  using build2::config::variable_origin;

  string
  serialize_cmdline (const string& name, const optional<build2::names>& value)
  {
    using namespace build2;

    string r (name + '=');

    if (!value)
      r += "[null]";
    else
    {
      if (!value->empty ())
      {
        // Note: we need to use command-line (effective) quoting.
        //
        ostringstream os;
        to_stream (os, *value, quote_mode::effective, '@');
        r += os.str ();
      }
    }

    return r;
  }

  void package_configuration::
  print (diag_record& dr,
         const char* indent,
         const dependent_config_variable_values* ovrs) const
  {
    bool first (true);
    for (const config_variable_value& v: *this)
    {
      if (v.origin != variable_origin::buildfile &&
          v.origin != variable_origin::override_)
        continue;

      if (first)
        first = false;
      else
        dr << '\n';

      dr << indent;

      if (ovrs != nullptr && v.origin == variable_origin::buildfile)
      {
        if (const dependent_config_variable_value* ov = ovrs->find (v.name))
        {
          dr << ov->serialize_cmdline () << " (set by " << ov->dependent << ')';
          continue;
        }
      }

      dr << v.serialize_cmdline () << " (";

      if (v.origin == variable_origin::buildfile)
        dr << "set by " << *v.dependent;
      else
        dr << "user configuration";

      dr << ')';
    }
  }

  void
  to_checksum (sha256& cs, const config_variable_value& v)
  {
    using namespace build2;

    cs.append (v.name);
    cs.append (static_cast<uint8_t> (v.origin));
    if (v.type)
      cs.append (*v.type);

    if (v.origin != variable_origin::undefined)
    {
      if (v.value)
        for (const name& n: *v.value)
          to_checksum (cs, n);

      if (v.origin == variable_origin::buildfile)
      {
        cs.append (v.dependent->string ());
        cs.append (v.confirmed);
      }
    }
  }

  optional<bool>
  negotiate_configuration (
    package_configurations& cfgs,
    package_skeleton& dept,
    pair<size_t, size_t> pos,
    const small_vector<reference_wrapper<package_skeleton>, 1>& depcs,
    bool has_alt)
  {
    assert (!dept.system);

    pos.first--; pos.second--; // Convert to 0-base.

    const dependency_alternative& da (
      dept.available->dependencies[pos.first][pos.second]);

    assert (da.require || da.prefer);

    // Step 1: save a snapshot of the old configuration while unsetting values
    // that have this dependent as the originator and reloading the defaults.
    //
    // The idea behind unsetting values previously (first) set by this
    // dependent is to allow it to "change its mind" based on other changes in
    // the configuration (e.g., some expensive feature got enabled by another
    // dependent which this dependent might as well use).
    //
    // This works well if the default values of configuration variables are
    // independent. However, consider this example:
    //
    // dependency:
    //
    //   config [bool] config.foo.x ?= false
    //   config [bool] config.foo.buf ?= ($config.foo.x ? 8196 : 4096)
    //
    // dependent:
    //
    //   config.foo.x = true
    //   config.foo.buf = ($config.foo.buf < 6144 ? 6144 : $config.foo.buf)
    //
    // Here if we unset both x and buf to their defaults, we will get an
    // incorrect result.
    //
    // The long-term solution here is to track dependencies between
    // configuration variables (which we can do as part of the config
    // directive via our parser::lookup_variable() hook and save this
    // information in the config module's saved_variables list). Then, we
    // "levelize" all the variables and have an inner "refinement" loop over
    // these levels. Specifically, we first unset all of them. Then we unset
    // all except the first level (which contains configuration variables that
    // don't depend on any others). And so on.
    //
    // And until we implement this, we expect the dependent to take such
    // configuration variable dependencies into account. For example:
    //
    // config.foo.x = true
    // config.foo.buf = ($config.foo.buf < 6144
    //                   ? ($config.foo.x ? 8196 : 6144)
    //                   : $config.foo.buf)
    //
    // Another issue with this "originating dependent" logic is that it will
    // be tricky to scale to containers where we would need to track
    // originating dependents for individual elements of a value rather than
    // the whole value as we do now. As an example, consider some "set of
    // enabled backends" configuration variable. Technically, this doesn't
    // seem insurmountable if we make some assumptions (e.g., if a value
    // contains multiple elements, then it is such a set-like value; or
    // we could use actual type names).
    //
    // For now we recommend to use multiple bool configurations to handle such
    // cases (and, in fact, we currently don't have any set/map-like types,
    // which we may want to add in the future). However, one could come up
    // with some open-ended configuration list that will be difficult to
    // support with bool. For example, we may need to support a set of buffer
    // sizes or some such.
    //
    // Our assumptions regarding require:
    //
    // - Can only set bool configuration variables and only to true.
    //
    // - Should not have any conditions on the state of other configuration
    //   variables, including their origin (but can have other conditions,
    //   for example on the target platform).
    //
    // This means that we don't need to set the default values, but will need
    // the type information as well as overrides. So what we will do is only
    // call reload_defaults() for the first time to load types/override. Note
    // that this assumes the set of configuration variables cannot change
    // based on the values of other configuration variables (we have a note
    // in the manual instructing the user not to do this).
    //
    // The dependency could also be system in which case there could be no
    // skeleton information to load the types/defaults from. In this case we
    // can handle require in the "lax mode" (based on the above assumptions)
    // but not prefer.
    //
    // While at it, also collect the configurations to pass to dependent's
    // evaluate_*() calls.
    //
    dependent_config_variable_values old_cfgs;
    package_skeleton::dependency_configurations depc_cfgs;
    depc_cfgs.reserve (depcs.size ());

    for (package_skeleton& depc: depcs)
    {
      package_configuration& cfg (cfgs[depc.package]);

      for (config_variable_value& v: cfg)
      {
        if (v.origin == variable_origin::buildfile)
        {
          if (*v.dependent == dept.package)
          {
            old_cfgs.push_back (
              dependent_config_variable_value {
                v.name, move (v.value), move (*v.dependent), v.has_alternative});

            // Note that we will not reload it to default in case of require.
            //
            v.undefine ();
          }
          else
            old_cfgs.push_back (
              dependent_config_variable_value {
                v.name, v.value, *v.dependent, v.has_alternative});
        }
      }

      if (depc.available == nullptr)
      {
        assert (depc.system);

        if (da.prefer)
          fail << "unable to negotiate configuration for system dependency "
               << depc.package << " without configuration information" <<
            info << "consider specifying system dependency version that has "
               << "corresponding available package" <<
            info << "dependent " << dept.package << " has prefer/accept clauses "
               << "that cannot be evaluated without configuration information";

        if (!cfg.system)
        {
          // Note that we still need the overrides.
          //
          depc.load_overrides (cfg);
          cfg.system = true;
        }

        continue;
      }
      else
        assert (!cfg.system);

      if (da.prefer || cfg.empty ())
        depc.reload_defaults (cfg);
    }

    // Note that we need to collect the dependency configurations as a
    // separate loop so that the stored references are not invalidated by
    // operator[] (which is really a push_back() into a vector).
    //
    for (package_skeleton& depc: depcs)
      depc_cfgs.push_back (cfgs[depc.package]);

    // Step 2: execute the prefer/accept or requires clauses.
    //
    if (!(da.require
          ? dept.evaluate_require (depc_cfgs, *da.require, pos, has_alt)
          : dept.evaluate_prefer_accept (depc_cfgs,
                                         *da.prefer, *da.accept, pos,
                                         has_alt)))
    {
      if (has_alt)
        return nullopt;

      diag_record dr (fail);

      dr << "unable to negotiate acceptable configuration with dependent "
         << dept.package << " for dependencies ";

      for (size_t i (0); i != depcs.size (); ++i)
        dr << (i == 0 ? "" : ", ") << depcs[i].get ().package;

      dr << info << "configuration before negotiation:\n";

      // Note that we won't print this dependent's values (which we have unset
      // above), but that seems like a good thing since they are not the cause
      // of this impasse.
      //
      for (const package_configuration& cfg: depc_cfgs)
        cfg.print (dr, "    "); // Note 4 spaces since in nested info.
    }

    // Check if anything changed by comparing to entries in old_cfgs.
    //
    // While at it, also detect if we have any changes where one dependent
    // overrides a value set by another dependent (see below).
    //
    bool cycle (false);
    {
      optional<size_t> n (0); // Number of unchanged.

      for (package_skeleton& depc: depcs)
      {
        package_configuration& cfg (cfgs[depc.package]);

        for (config_variable_value& v: cfg)
        {
          if (v.origin == variable_origin::buildfile)
          {
            if (const dependent_config_variable_value* ov =
                  old_cfgs.find (v.name))
            {
              if (ov->value == v.value)
              {
                // If the value hasn't change, so shouldn't the originating
                // dependent.
                //
                assert (ov->dependent == *v.dependent);

                if (n)
                  ++*n;

                continue;
              }
              else
              {
                // Note that it's possible the same dependent overrides its
                // old value (e.g., because a conditional default changed to a
                // better value).
                //
                if (ov->dependent != *v.dependent)
                  cycle = true;
              }
            }

            n = nullopt;

            if (cycle)
              break;
          }
        }

        if (!n && cycle)
          break;
      }

      // If we haven't seen any changed and we've seen the same number, then
      // nothing has changed.
      //
      if (n && *n == old_cfgs.size ())
        return false;
    }

    // Besides the dependent returning false from its accept clause, there is
    // another manifestation of the inability to negotiate an acceptable
    // configuration: two dependents keep changing the same configuration to
    // mutually unacceptable values. To detect this, we need to look for
    // negotiation cycles.
    //
    // Specifically, given a linear change history in the form:
    //
    //   O->N ... O->N ... O->N
    //
    // We need to look for a possibility of turning it into a cycle:
    //
    //   O->N ... O->N
    //    \   ...   /
    //
    // Where O->N is a change that involves one dependent overriding a value
    // set by another dependent and `...` are identical history segments.
    //
    if (!cycle)
      return true;

    // Populate new_cfgs.
    //
    dependent_config_variable_values new_cfgs;
    for (package_skeleton& depc: depcs)
    {
      package_configuration& cfg (cfgs[depc.package]);

      for (config_variable_value& v: cfg)
      {
        if (v.origin == variable_origin::buildfile)
        {
          new_cfgs.push_back (
            dependent_config_variable_value {
              v.name, v.value, *v.dependent, v.has_alternative});
        }
      }
    }

    // Sort both.
    //
    {
      auto cmp = [] (const dependent_config_variable_value& x,
                     const dependent_config_variable_value& y)
      {
        return x.name < y.name;
      };

      sort (old_cfgs.begin (), old_cfgs.end (), cmp);
      sort (new_cfgs.begin (), new_cfgs.end (), cmp);
    }

    // Look backwards for identical O->N changes and see if we can come
    // up with two identical segments between them.
    //
    cycle = false;

    auto& change_history (cfgs.change_history_);

    for (size_t n (change_history.size ()), i (n); i != 0; i -= 2)
    {
      if (change_history[i - 2] == old_cfgs &&
          change_history[i - 1] == new_cfgs)
      {
        size_t d (n - i); // Segment length.

        // See if there is an identical segment before this that also starts
        // with O->N.
        //
        if (i < 2 + d + 2)
          break; // Not long enough to possibly find anything.

        size_t j (i - 2 - d); // Earlier O->N.

        if (change_history[j - 2] == old_cfgs &&
            change_history[j - 1] == new_cfgs)
        {
          if (equal (change_history.begin () + j,
                     change_history.begin () + j + d,
                     change_history.begin () + i))
          {
            cycle = true;
            break;
          }
        }

        // Otherwise, keep looking for a potentially longer segment.
      }
    }

    if (!cycle)
    {
      change_history.push_back (move (old_cfgs));
      change_history.push_back (move (new_cfgs));

      return true;
    }

    if (has_alt)
      return nullopt;

    // Analyze the O->N changes and determine the problematic dependent(s).
    // Do we actually know for sure they are all problematic? Well, they
    // repeatedly changed the values to the ones we don't like, so I guess so.
    //
    // If it's the other dependent that has an alternative, then we let the
    // negotiation continue for one more half-cycle at which point it will be
    // while negotiating the configuration of the other dependent that we will
    // (again) detect this cycle.
    //
    small_vector<reference_wrapper<const package_key>, 1> depts;
    for (const dependent_config_variable_value& nv: new_cfgs)
    {
      if (nv.dependent == dept.package)
      {
        if (const dependent_config_variable_value* ov = old_cfgs.find (nv.name))
        {
          if (ov->value != nv.value && ov->dependent != nv.dependent)
          {
            if (find_if (depts.begin (), depts.end (),
                         [ov] (reference_wrapper<const package_key> pk)
                         {
                           return ov->dependent == pk.get ();
                         }) == depts.end ())
            {
              if (ov->has_alternative)
              {
                change_history.push_back (move (old_cfgs));
                change_history.push_back (move (new_cfgs));

                return true;
              }

              depts.push_back (ov->dependent);
            }
          }
        }
      }
    }

    diag_record dr (fail);

    dr << "unable to negotiate acceptable configuration between dependents "
       << dept.package;

    for (const package_key& d: depts)
      dr << ", " << d;

    dr << " for dependencies ";

    for (size_t i (0); i != depcs.size (); ++i)
      dr << (i == 0 ? "" : ", ") << depcs[i].get ().package;

    dr << info << "configuration before negotiation:\n";
    for (const package_configuration& cfg: depc_cfgs)
      cfg.print (dr, "    ", &old_cfgs);

    dr << info << "configuration after negotiation:\n";
    for (const package_configuration& cfg: depc_cfgs)
      cfg.print (dr, "    ");

    dr << endf;
  }
}
