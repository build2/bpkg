// file      : bpkg/utility.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_UTILITY_HXX
#define BPKG_UTILITY_HXX

#include <memory>   // make_shared()
#include <string>   // to_string()
#include <utility>  // move(), forward(), declval(), make_pair()
#include <cassert>  // assert()
#include <iterator> // make_move_iterator()

#include <butl/ft/lang>

#include <butl/utility> // casecmp(), reverse_iterate(), etc

#include <butl/filesystem>

#include <bpkg/types.hxx>
#include <bpkg/version.hxx>

namespace bpkg
{
  using std::move;
  using std::forward;
  using std::declval;

  using std::make_pair;
  using std::make_shared;
  using std::make_move_iterator;
  using std::to_string;

  // <butl/utility>
  //
  using butl::casecmp;
  using butl::reverse_iterate;

  using butl::exception_guard;
  using butl::make_exception_guard;

  // Widely-used paths.
  //
  extern const dir_path bpkg_dir;  // .bpkg/
  extern const dir_path certs_dir; // .bpkg/certs/

  // Y/N prompt. The def argument, if specified, should be either 'y'
  // or 'n'. It is used as the default answer, in case the user just
  // hits enter. Issue diagnostics and throw failed if no answer could
  // be extracted from STDOUT (e.g., because it was closed).
  //
  bool
  yn_prompt (const char* prompt, char def = '\0');

  // Filesystem.
  //
  bool
  exists (const path&);

  bool
  exists (const dir_path&);

  bool
  empty (const dir_path&);

  void
  mk (const dir_path&);

  void
  mk_p (const dir_path&);

  void
  rm (const path&);

  void
  rm_r (const dir_path&, bool dir = true);

  using auto_rm = butl::auto_rmfile;
  using auto_rm_r = butl::auto_rmdir;

  // Process.
  //
  // By default the process command line is printed for verbosity >= 2
  // (essential command lines).
  //
  // If fallback is specified, then this directory is searched for the
  // executable as a last resort.
  //
  void
  run (const char* args[], const dir_path& fallback = dir_path ());

  inline void
  run (cstrings& args, const dir_path& fallback = dir_path ())
  {
    run (args.data (), fallback);
  }

  // Directory extracted from argv[0] (i.e., this process' recall directory)
  // or empty if there is none. Can be used as a search fallback.
  //
  extern dir_path exec_dir;

  // Run build2, mapping verbosity levels. If quiet is true, then run build2
  // quiet if our verbosity level is 1. Common vars (cvars) are set on the
  // configuration scope.
  //
  class common_options;

  const char*
  name_b (const common_options&);

  void
  run_b (const common_options&,
         const dir_path& configuration,
         const string& buildspec,
         bool quiet = false,
         const strings& pvars = strings (),
         const strings& cvars = strings ());
}

#endif // BPKG_UTILITY_HXX
