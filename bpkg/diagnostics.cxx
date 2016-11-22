// file      : bpkg/diagnostics.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/diagnostics>

#include <cstring>  // strchr()
#include <iostream>

#include <odb/statement.hxx>

#include <butl/process>

using namespace std;
using namespace butl;

namespace bpkg
{
  // print_process
  //
  void
  print_process (const char* const args[], size_t n)
  {
    diag_record r (text);
    print_process (r, args, n);
  }

  struct process_args
  {
    const char* const* a;
    size_t n;
  };

  inline static ostream&
  operator<< (ostream& o, const process_args& p)
  {
    process::print (o, p.a, p.n);
    return o;
  }

  void
  print_process (diag_record& r, const char* const args[], size_t n)
  {
    r << process_args {args, n};
  }

  // Diagnostics verbosity level.
  //
  uint16_t verb;

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

  // trace
  //
  void trace_mark_base::
  prepare (odb::connection&, const odb::statement& s)
  {
    if (verb >= 6)
      static_cast<trace_mark&> (*this) << "PREPARE " << s.text ();
  }

  void trace_mark_base::
  execute (odb::connection&, const char* stmt)
  {
    if (verb >= 5)
      static_cast<trace_mark&> (*this) << stmt;
  }

  void trace_mark_base::
  deallocate (odb::connection&, const odb::statement& s)
  {
    if (verb >= 6)
      static_cast<trace_mark&> (*this) << "DEALLOCATE " << s.text ();
  }

  const basic_mark error ("error");
  const basic_mark warn  ("warning");
  const basic_mark info  ("info");
  const basic_mark text  (nullptr);
  const fail_mark  fail  ("error");
  const fail_end   endf;
}
