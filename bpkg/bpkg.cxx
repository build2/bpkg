// file      : bpkg/bpkg.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <iostream>
#include <exception>

#include <bpkg/diagnostics>

using namespace std;
using namespace bpkg;

int
main ()
{
  tracer trace ("main");

  try
  {
    // Trace verbosity.
    //
    verb = 0;
  }
  catch (const failed&)
  {
    return 1; // Diagnostics has already been issued.
  }
  /*
  catch (const std::exception& e)
  {
    error << e.what ();
    return 1;
  }
  */
}
