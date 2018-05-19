// file      : bpkg/archive.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_ARCHIVE_HXX
#define BPKG_ARCHIVE_HXX

#include <libbutl/process.mxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/common-options.hxx>

namespace bpkg
{
  // Return the package directory based on the package archive.
  //
  dir_path
  package_dir (const path& archive);

  // Start the process of extracting the specified file from the archive. If
  // error is false, then redirect STDERR to /dev/null (this can be used, for
  // example, to suppress diagnostics).
  //
  // Return a pair of processes that form a pipe. Wait on the second first.
  //
  pair<butl::process, butl::process>
  start_extract (const common_options&,
                 const path& archive,
                 const path& file,
                 bool error = true);

  // Start as above and then extract the file content as a string.
  //
  string
  extract (const common_options&, const path& archive, const path& file);
}

#endif // BPKG_ARCHIVE_HXX
