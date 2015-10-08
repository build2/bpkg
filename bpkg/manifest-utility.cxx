// file      : bpkg/manifest-utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/manifest-utility>

#include <stdexcept> // invalid_argument

#include <bpkg/diagnostics>

using namespace std;

namespace bpkg
{
  string
  parse_package_name (const char* s)
  {
    using traits = string::traits_type;

    size_t n (traits::length (s));

    if (const char* p = traits::find (s, n, '/'))
      n = static_cast<size_t> (p - s);

    if (n == 0)
      fail << "empty package name in '" << s << "'";

    return string (s, n);
  }

  version
  parse_package_version (const char* s)
  {
    using traits = string::traits_type;

    if (const char* p = traits::find (s, traits::length (s), '/'))
    {
      if (*++p == '\0')
        fail << "empty package version in '" << s << "'";

      try
      {
        return version (p);
      }
      catch (const invalid_argument& e)
      {
        fail << "invalid package version '" << p << "': " << e.what ();
      }
    }

    return version ();
  }

  repository_location
  parse_location (const char* s)
  try
  {
    repository_location rl (s, repository_location ());

    if (rl.relative ()) // Throws if the location is empty.
      rl = repository_location (
        dir_path (s).complete ().normalize ().string ());

    return rl;
  }
  catch (const invalid_argument& e)
  {
    error << "invalid repository location '" << s << "': " << e.what ();
    throw failed ();
  }
}
