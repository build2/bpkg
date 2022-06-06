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
    // While at it, also collect the configurations to pass to dependent's
    // evaluate_*() calls.
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

      depc_cfgs.push_back (cfg);
    }

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

            if (i == old_cfgs.end () ||
                i->value != v.value  ||
                i->dependent != *v.dependent)
            {
              n = nullopt;
              break;
            }

            ++*n;
          }
        }

        if (!n)
          break;
      }

      // If we haven't seen any changed and we've seen the same number, then
      // nothing has changed.
      //
      if (n && *n == old_cfgs.size ())
        return false;
    }


    // @@ TODO: look for cycles in change history.
    // @@ TODO: save in change history.
    //
    /*
    dependent_config_variable_values new_cfgs; // @@ TODO.

    old_cfgs.sort ();
    new_cfgs.sort ();
    */

    return true;
  }
}
