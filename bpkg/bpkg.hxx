// file      : bpkg/bpkg.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_BPKG_HXX
#define BPKG_BPKG_HXX

// Embedded build system driver.
//
#include <libbuild2/context.hxx>
#include <libbuild2/scheduler.hxx>
#include <libbuild2/file-cache.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/common-options.hxx>

namespace bpkg
{
  // Initialized by build2_parse_cmdline() and build2_init().
  //
  extern optional<strings>      build2_cmd_vars;

  // Initialized by build2_init().
  //
  extern build2::scheduler      build2_sched;
  extern build2::global_mutexes build2_mutexes;
  extern build2::file_cache     build2_fcache;

  // Use build2_cmd_vars to check if already parsed.
  //
  void
  build2_parse_cmdline (const common_options&);

  // Use build2_sched.started() to check if already initialized. Note that the
  // scheduler is pre-tuned for serial execution.
  //
  void
  build2_init (const common_options&);
}

#endif // BPKG_BPKG_HXX
