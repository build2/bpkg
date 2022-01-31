// file      : bpkg/archive.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_ARCHIVE_HXX
#define BPKG_ARCHIVE_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/common-options.hxx>

namespace bpkg
{
  // Return the package directory based on the package archive.
  //
  dir_path
  package_dir (const path& archive);

  // Start the process of extracting the archive to the specified directory.
  //
  pair<process, process>
  start_extract (const common_options&,
                 const path& archive,
                 const dir_path&);

  // Start the process of extracting the specified file from the archive to
  // the process' stdout. If diag is false, then redirect stderr to /dev/null
  // (this can be used, for example, to suppress diagnostics). Note that in
  // this case process errors (like unable to start) are still reported.
  //
  // Return a pair of processes that form a pipe. Wait on the second first.
  //
  pair<process, process>
  start_extract (const common_options&,
                 const path& archive,
                 const path& file,
                 bool diag = true);

  // Start as above and then extract the file content as a string. If diag is
  // false, then don't issue diagnostics about the reason why the file can't
  // be extracted (not present, the archive is broken, etc).
  //
  string
  extract (const common_options&,
           const path& archive,
           const path& file,
           bool diag = true);

  // Start the processes similar to the above functions but execute tar in the
  // archive contents listing mode (-t) and then parse its stdout as a list of
  // paths (one per line). If diag is false, then don't issue diagnostics
  // about the reason why the contents can't be obtained (the archive is
  // broken, etc).
  //
  paths
  archive_contents (const common_options&,
                    const path& archive,
                    bool diag = true);
}

#endif // BPKG_ARCHIVE_HXX
