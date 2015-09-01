// file      : bpkg/diagnostics.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/diagnostics>

#include <iostream>

using namespace std;

namespace bpkg
{
  // Trace verbosity level.
  //
  uint16_t verb;

  // Diagnostic facility, base infrastructure.
  //
  ostream* diag_stream = &cerr;

  diag_record::
  ~diag_record () noexcept(false)
  {
    // Don't flush the record if this destructor was called as part of
    // the stack unwinding. Right now this means we cannot use this
    // mechanism in destructors, which is not a big deal, except for
    // one place: exception_guard. So for now we are going to have
    // this ugly special check which we will be able to get rid of
    // once C++17 uncaught_exceptions() becomes available.
    //
    if (!empty_ && (!uncaught_exception () /*|| exception_unwinding_dtor*/))
    {
      *diag_stream << os_.str () << endl;

      if (epilogue_ != nullptr)
        epilogue_ (*this); // Can throw.
    }
  }

  // Diagnostic facility, project specifics.
  //

  void simple_prologue_base::
  operator() (const diag_record& r) const
  {
    if (type_ != nullptr)
      r << type_ << ": ";

    if (name_ != nullptr)
      r << name_ << ": ";
  }

  void location_prologue_base::
  operator() (const diag_record& r) const
  {
    r << loc_.file << ':' << loc_.line << ':' << loc_.column << ": ";

    if (type_ != nullptr)
      r << type_ << ": ";

    if (name_ != nullptr)
      r << name_ << ": ";
  }

  const basic_mark error ("error");
  const basic_mark warn ("warning");
  const basic_mark info ("info");
  const basic_mark text (nullptr);
  const fail_mark<failed> fail;
}
