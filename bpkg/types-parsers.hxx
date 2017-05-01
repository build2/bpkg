// file      : bpkg/types-parsers.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

// CLI parsers, included into the generated source files.
//

#ifndef BPKG_TYPES_PARSERS_HXX
#define BPKG_TYPES_PARSERS_HXX

#include <bpkg/types.hxx>
#include <bpkg/options-types.hxx>

namespace bpkg
{
  namespace cli
  {
    class scanner;

    template <typename T>
    struct parser;

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
  }
}

#endif // BPKG_TYPES_PARSERS_HXX
