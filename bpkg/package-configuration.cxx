// file      : bpkg/package-configuration.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package-configuration.hxx>

#include <bpkg/package-skeleton.hxx>

namespace bpkg
{
  using build2::config::variable_origin;

  bool
  up_negotiate_configuration (
    package_configurations& cfgs,
    package_skeleton& dept,
    pair<size_t, size_t> pos,
    const small_vector<reference_wrapper<package_skeleton>, 1>& depcs)
  {
    pos.first--; pos.second--; // Convert to 0-base.

    const dependency_alternative& da (
      dept.available.get ().dependencies[pos.first][pos.second]);

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
    // While at it, also collect the configurations to pass to dependent's
    // evaluate_*() calls.
    //
    dependent_config_variable_values old_cfgs;
    package_skeleton::dependency_configurations depc_cfgs;
    depc_cfgs.reserve (depcs.size ());

    for (package_skeleton& depc: depcs)
    {
      package_configuration& cfg (cfgs[depc.key]);

      for (config_variable_value& v: cfg)
      {
        if (v.origin == variable_origin::buildfile)
        {
          if (*v.dependent == dept.key)
          {
            old_cfgs.push_back (
              dependent_config_variable_value {
                v.name, move (v.value), move (*v.dependent)});

            // Note that we will not reload it to default in case of require.
            //
            v.origin = variable_origin::undefined;
            v.value = nullopt;
            v.dependent = nullopt;
          }
          else
            old_cfgs.push_back (
              dependent_config_variable_value {v.name, v.value, *v.dependent});
        }
      }

      if (da.prefer || cfg.empty ())
        depc.reload_defaults (cfg);
    }

    // Note that we need to collect the dependency configurations as a
    // separate loop so that the stored references are not invalidated by
    // operator[] (which is really a push_back() into a vector).
    //
    for (package_skeleton& depc: depcs)
      depc_cfgs.push_back (cfgs[depc.key]);

    // Step 2: execute the prefer/accept or requires clauses.
    //
    if (!(da.require
          ? dept.evaluate_require (depc_cfgs, *da.require, pos.first)
          : dept.evaluate_prefer_accept (depc_cfgs,
                                         *da.prefer, *da.accept, pos.first)))
    {
      fail << "unable to negotiate acceptable configuration"; // @@ TODO
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
        package_configuration& cfg (cfgs[depc.key]);

        for (config_variable_value& v: cfg)
        {
          if (v.origin == variable_origin::buildfile)
          {
            auto i (find_if (old_cfgs.begin (), old_cfgs.end (),
                             [&v] (const dependent_config_variable_value& o)
                             {
                               return v.name == o.name;
                             }));

            if (i != old_cfgs.end ())
            {
              if (i->value == v.value)
              {
                // If the value hasn't change, so shouldn't the originating
                // dependent.
                //
                assert (i->dependent == *v.dependent);

                if (n)
                  ++*n;

                continue;
              }
              else
              {
                assert (i->dependent != *v.dependent);
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
      package_configuration& cfg (cfgs[depc.key]);

      for (config_variable_value& v: cfg)
      {
        if (v.origin == variable_origin::buildfile)
        {
          new_cfgs.push_back (
            dependent_config_variable_value {v.name, v.value, *v.dependent});
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

    if (cycle)
    {
      // @@ TODO
      //
      // Here we can analyze the O->N change history and determine the other
      // problematic dependent(s). Do we actually know for sure they are all
      // problematic? Well, they repeatedly changed the values so I guess so.
      //
      fail << "unable to negotiate acceptable configuration (cycle)";
    }

    change_history.push_back (move (old_cfgs));
    change_history.push_back (move (new_cfgs));

    return true;
  }
}