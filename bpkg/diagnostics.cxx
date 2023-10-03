// file      : bpkg/diagnostics.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/diagnostics.hxx>

#include <iostream>

#include <odb/statement.hxx>

#include <libbutl/process.hxx>    // process_args
#include <libbutl/process-io.hxx> // operator<<(ostream, process_*)

#include <bpkg/utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  // print_process
  //
  void
  print_process (const char* const args[], size_t n)
  {
    diag_record dr (text);
    print_process (dr, args, n);
  }

  void
  print_process (diag_record& dr, const char* const args[], size_t n)
  {
    dr << process_args {args, n};
  }

  void
  print_process (const process_env& pe, const char* const args[], size_t n)
  {
    diag_record dr (text);
    print_process (dr, pe, args, n);
  }

  void
  print_process (diag_record& dr,
                 const process_env& pe, const char* const args[], size_t n)
  {
    if (pe.env ())
      dr << pe << ' ';

    dr << process_args {args, n};
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
    if (!loc_.empty ())
    {
      r << loc_.file << ':';

      if (loc_.line != 0)
      {
        r << loc_.line << ':';

        if (loc_.column != 0)
          r << loc_.column << ':';
      }

      r << ' ';
    }

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
    if (verb >= 6)
      static_cast<trace_mark&> (*this) << stmt;
  }

  void trace_mark_base::
  deallocate (odb::connection&, const odb::statement& s)
  {
    if (verb >= 6)
      static_cast<trace_mark&> (*this) << "DEALLOCATE " << s.text ();
  }

  // tracer
  //
  void tracer::
  operator() (const char* const args[], size_t n) const
  {
    if (verb >= 3)
    {
      diag_record dr (*this);
      print_process (dr, args, n);
    }
  }

  const basic_mark error ("error");
  const basic_mark warn  ("warning");
  const basic_mark info  ("info");
  const basic_mark text  (nullptr, nullptr, nullptr, nullptr); // No frame.
  const fail_mark  fail  ("error");
  const fail_end   endf;
}
