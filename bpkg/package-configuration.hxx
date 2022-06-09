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

    // Variable type name with absent signifying untyped.
    //
    optional<string> type;

    // If origin is not undefined, then this is the reversed variable value
    // with absent signifying NULL.
    //
    optional<build2::names> value;

    // If origin is buildfile, then this is the "originating dependent" which
    // first set this variable to this value.
    //
    optional<package_key> dependent;

    // If origin is buildfile, then this flag indicates whether the
    // originating dependent has been encountered during the negotiation
    // retry.
    //
    bool confirmed;

  public:
    void
    undefine ()
    {
      origin = build2::config::variable_origin::undefined;
      value = nullopt;
      dependent = nullopt;
      confirmed = false;
    }

    // Serialize the variable value as a command line override.
    //
    string
    serialize_cmdline () const;
  };

  void
  to_checksum (sha256&, const config_variable_value&);

  // A subset of config_variable_value for variable values set by the
  // dependents (origin is buildfile). Used to track change history.
  //
  struct dependent_config_variable_value
  {
    string                  name;
    optional<build2::names> value;
    package_key             dependent;
  };

  inline bool
  operator== (const dependent_config_variable_value& x,
              const dependent_config_variable_value& y)
  {
    return x.name == y.name && x.value == y.value && x.dependent == y.dependent;
  }

  class package_configuration: public vector<config_variable_value>
  {
  public:
    package_key package;

    explicit
    package_configuration (package_key p): package (move (p)) {}

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

  using dependent_config_variable_values =
    small_vector<dependent_config_variable_value, 1>;

  class package_configurations: public small_vector<package_configuration, 1>
  {
  public:
    // Note: may invalidate references.
    //
    package_configuration&
    operator[] (const package_key& p)
    {
      auto i (find_if (begin (), end (),
                       [&p] (const package_configuration& pc)
                       {
                         return pc.package == p;
                       }));
      if (i != end ())
        return *i;

      push_back (package_configuration (p));
      return back ();
    }

    void
    clear ()
    {
      small_vector<package_configuration, 1>::clear ();
      change_history_.clear ();
    }

    // Implementation details.
    //
  public:
    // Note: dependent_config_variable_values must be sorted by name.
    //
    small_vector<dependent_config_variable_values, 2> change_history_;
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
