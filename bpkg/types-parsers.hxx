// file      : bpkg/types-parsers.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

// CLI parsers, included into the generated source files.
//

#ifndef BPKG_TYPES_PARSERS_HXX
#define BPKG_TYPES_PARSERS_HXX

#include <libbpkg/manifest.hxx>

#include <bpkg/types.hxx>

#include <bpkg/common-options.hxx> // bpkg::cli namespace
#include <bpkg/options-types.hxx>

namespace bpkg
{
  namespace cli
  {
    template <>
    struct parser<url>
    {
      static void
      parse (url&, bool&, scanner&);

      static void
      merge (url& b, const url& a) {b = a;}
    };

    template <>
    struct parser<path>
    {
      static void
      parse (path&, bool&, scanner&);

      static void
      merge (path& b, const path& a) {b = a;}
    };

    template <>
    struct parser<dir_path>
    {
      static void
      parse (dir_path&, bool&, scanner&);

      static void
      merge (dir_path& b, const dir_path& a) {b = a;}
    };

    template <>
    struct parser<auth>
    {
      static void
      parse (auth&, bool&, scanner&);

      static void
      merge (auth& b, const auth& a) {b = a;}
    };

    template <>
    struct parser<repository_type>
    {
      static void
      parse (repository_type&, bool&, scanner&);

      static void
      merge (repository_type& b, const repository_type& a) {b = a;}
    };

    template <const char* const* Q, typename V>
    struct parser<qualified_option<Q, V>>
    {
      static void
      parse (qualified_option<Q, V>&, bool&, scanner&);

      static void
      merge (qualified_option<Q, V>&, const qualified_option<Q, V>&);
    };
  }
}

#include <bpkg/types-parsers.txx>

#endif // BPKG_TYPES_PARSERS_HXX
