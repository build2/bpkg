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
    dependent_config_variable_values old_cfgs;

    // Step 1: save a snapshot of the old configuration while unsetting values
    // that have this dependent as the originator and reloading the defaults.
    //
    // While at it, also collect the configurations to pass to dependent's
    // evaluate_*() calls.
    //
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

            v.origin = variable_origin::undefined;
            v.dependent = nullopt;
          }
          else
            old_cfgs.push_back (
              dependent_config_variable_value {v.name, v.value, *v.dependent});
        }
      }

      depc.reload_defaults (cfg);

      depc_cfgs.push_back (cfg);
    }

    // Step 2: execute the prefer/accept or requires clauses.
    //
    pos.first--; pos.second--; // Convert to 0-base.

    const dependency_alternative& da (
      dept.available.get ().dependencies[pos.first][pos.second]);

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
