// file      : bpkg/package-configuration.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PACKAGE_CONFIGURATION_HXX
#define BPKG_PACKAGE_CONFIGURATION_HXX

#include <map>

#include <libbuild2/types.hxx>        // build2::names
#include <libbuild2/config/types.hxx> // build2::config::variable_origin

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>

using namespace std;

namespace bpkg
{
  class package_skeleton;

  struct config_variable_value
  {
    string name;

    // The variable_origin values have the following meaning:
    //
    // default    -- default value from the config directive
    // buildfile  -- dependent configuration (config_source::dependent)
    // override   -- user configuration      (config_source::user)
    // undefined  -- none of the above
    //
    build2::config::variable_origin origin;

    // If origin is not undefined, then this is the reversed variable value
    // with absent signifying NULL.
    //
    optional<build2::names> value;

    // Variable type name with absent signifying untyped.
    //
    optional<string> type;

    // If origin is buildfile, then this is the "originating dependent" which
    // first set this variable to this value.
    //
    optional<package_key> dependent;

    // Value version (used internally by package_skeleton).
    //
    size_t version;
  };

  class package_configuration: public vector<config_variable_value>
  {
  public:
    config_variable_value*
    find (const string& name)
    {
      auto i (find_if (begin (), end (),
                       [&name] (const config_variable_value& v)
                       {
                         return v.name == name;
                       }));
      return i != end () ? &*i : nullptr;
    }

    const config_variable_value*
    find (const string& name) const
    {
      auto i (find_if (begin (), end (),
                       [&name] (const config_variable_value& v)
                       {
                         return v.name == name;
                       }));
      return i != end () ? &*i : nullptr;
    }
  };


  // @@ Maybe redo as small_vector?
  //
  using package_configurations = map<package_key, package_configuration>;


  // A subset of config_variable_value for variable values set by the
  // dependents (origin is buildfile). Used to track change history.
  //
  struct dependent_config_variable_value
  {
    string                  name;
    optional<build2::names> value;
    package_key             dependent;
  };

  class dependent_config_variable_values:
    public small_vector<dependent_config_variable_value, 1>
  {
  public:
    /*
      @@
    void
    sort ();
    */
  };

  // Up-negotiate the configuration for the specified dependencies of the
  // specified dependent. Return true if the configuration has changed.
  //
  bool
  up_negotiate_configuration (
    package_configurations&,
    package_skeleton& dependent,
    pair<size_t, size_t> position,
    const small_vector<reference_wrapper<package_skeleton>, 1>& dependencies);
}

#endif // BPKG_PACKAGE_CONFIGURATION_HXX
