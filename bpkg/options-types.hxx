// file      : bpkg/options-types.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_OPTIONS_TYPES_HXX
#define BPKG_OPTIONS_TYPES_HXX

#include <map>
#include <cassert>
#include <utility> // move()

#include <libbutl/prefix-map.hxx>

#include <bpkg/types.hxx>

namespace bpkg
{
  using uuid_type = uuid;

  enum class auth
  {
    none,
    remote,
    all
  };

  enum class stdout_format
  {
    lines,
    json
  };

  enum class git_protocol_capabilities
  {
    dumb,  // No shallow clone support.
    smart, // Support for shallow clone, but not for unadvertised refs fetch.
    unadv  // Support for shallow clone and for unadvertised refs fetch.
  };

  using git_capabilities_map = butl::prefix_map<string,
                                                git_protocol_capabilities,
                                                '/'>;

  // Qualified options.
  //
  // An option that uses this type can have its values qualified using the
  // <qualifier>:<value> form, for example, '--option foo:bar'. An unqualified
  // value that contains a colon can be specified as qualified with an empty
  // qualifier, for example, '--option :http://example.org'. Unqualified
  // values apply to all the qualifiers in the order specified.
  //
  // The second template argument is a NULL-terminated list of valid qualifier
  // strings, for example:
  //
  // const char* option_qualifiers[] = {"foo", "bar", nullptr};
  //
  template <const char* const* Q, typename V>
  class qualified_option: public std::map<string, V>
  {
  public:
    using base_type = std::map<string, V>;

    template <typename T>
    explicit
    qualified_option (T v) {this->emplace (string (), V (std::move (v)));}

    qualified_option (): qualified_option (V ()) {}

    using base_type::operator[];

    const V&
    operator[] (const string& q) const
    {
      auto verify = [&q] ()
      {
        for (const char* const* p (Q); *p != nullptr; ++p)
        {
          if (q == *p)
            return true;
        }
        return q.empty ();
      };

      assert (verify ());

      typename base_type::const_iterator i (this->find (q));

      if (i == this->end ())
        i = this->find ("");

      assert (i != this->end ());
      return i->second;
    }
  };

  extern const char* openssl_commands[5]; // Clang bug requres explicit size.
}

#endif // BPKG_OPTIONS_TYPES_HXX
