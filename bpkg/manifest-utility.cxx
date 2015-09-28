// file      : bpkg/manifest-utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/manifest-utility>

#include <stdexcept> // invalid_argument

#include <bpkg/diagnostics>

using namespace std;

namespace bpkg
{
  version
  parse_version (const char* s)
  try
  {
    return version (s);
  }
  catch (const invalid_argument& e)
  {
    error << "invalid package version '" << s << "': " << e.what ();
    throw failed ();
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
