// file      : bpkg/types-parsers.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/types-parsers>

#include <bpkg/common-options> // bpkg::cli namespace

namespace bpkg
{
  namespace cli
  {
    void parser<dir_path>::
    parse (dir_path& x, bool& xs, scanner& s)
    {
      xs = true;
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const char* v (s.next ());

      try
      {
        x = dir_path (v);

        if (x.empty ())
          throw invalid_value (o, v);
      }
      catch (const invalid_path&)
      {
        throw invalid_value (o, v);
      }
    }
  }
}
