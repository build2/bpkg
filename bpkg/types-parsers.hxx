// file      : bpkg/types-parsers.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
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
    struct parser<path>
    {
      static void
      parse (path&, bool&, scanner&);
    };

    template <>
    struct parser<dir_path>
    {
      static void
      parse (dir_path&, bool&, scanner&);
    };

    template <>
    struct parser<auth>
    {
      static void
      parse (auth&, bool&, scanner&);
    };

    template <>
    struct parser<repository_type>
    {
      static void
      parse (repository_type&, bool&, scanner&);
    };

    template <const char* const* Q, typename V>
    struct parser<qualified_option<Q, V>>
    {
      static void
      parse (qualified_option<Q, V>&, bool&, scanner&);
    };
  }
}

#include <bpkg/types-parsers.txx>

#endif // BPKG_TYPES_PARSERS_HXX
