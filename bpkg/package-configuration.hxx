// file      : bpkg/package-configuration.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PACKAGE_CONFIGURATION_HXX
#define BPKG_PACKAGE_CONFIGURATION_HXX

#include <libbuild2/types.hxx>        // build2::names
#include <libbuild2/config/types.hxx> // build2::config::variable_origin

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>

using namespace std;

namespace bpkg
{
  class package_skeleton;

  // Serialize the variable value as a command line override.
  //
  string
  serialize_cmdline (const string& name, const optional<build2::names>& value);

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

    // If origin is buildfile and the originating dependent has been
    // encountered during the negotiation, then this flag indicates whether
    // this dependent has another dependency alternative.
    //
    // @@ Strictly speaking this is a property of the dependent and
    //    duplicating it here for each variable is quite dirty (and requires
    //    us to drag this through skeleton calls). Doing this properly,
    //    however, will likely require another map with the dependent as a
    //    key. Maybe one day.
    //
    bool has_alternative;

  public:
    void
    undefine ()
    {
      origin = build2::config::variable_origin::undefined;
      value = nullopt;
      dependent = nullopt;
      confirmed = false;
      has_alternative = false;
    }

    string
    serialize_cmdline () const
    {
      return bpkg::serialize_cmdline (name, value);
    }
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
    bool                    has_alternative;

  public:
    string
    serialize_cmdline () const
    {
      return bpkg::serialize_cmdline (name, value);
    }
  };

  inline bool
  operator== (const dependent_config_variable_value& x,
              const dependent_config_variable_value& y)
  {
    return x.name == y.name && x.value == y.value && x.dependent == y.dependent;
  }

  class dependent_config_variable_values:
    public small_vector<dependent_config_variable_value, 1>
  {
  public:
    const dependent_config_variable_value*
    find (const string& name) const
    {
      auto i (find_if (begin (), end (),
                       [&name] (const dependent_config_variable_value& v)
                       {
                         return v.name == name;
                       }));
      return i != end () ? &*i : nullptr;
    }
  };

  class package_configuration: public vector<config_variable_value>
  {
  public:
    package_key package;
    bool system = false; // True if system package without skeleton info.

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

    // Print buildfile and override configuration variable values as command
    // line overrides one per line with the specified indentation. After each
    // variable also print in parenthesis its origin. If overrides is not
    // NULL, then it is used to override the value/dependent information.
    //
    void
    print (diag_record&, const char* indent,
           const dependent_config_variable_values* overrides = nullptr) const;
  };

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

  // Negotiate the configuration for the specified dependencies of the
  // specified dependent. Return true if the configuration has changed.
  // Return absent if has_alternative is true and no acceptable configuration
  // could be negotiated.
  //
  optional<bool>
  negotiate_configuration (
    package_configurations&,
    package_skeleton& dependent,
    pair<size_t, size_t> position,
    const small_vector<reference_wrapper<package_skeleton>, 1>& dependencies,
    bool has_alternative);
}

#endif // BPKG_PACKAGE_CONFIGURATION_HXX
