// file      : bpkg/checksum.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_CHECKSUM_HXX
#define BPKG_CHECKSUM_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/common-options.hxx>

namespace bpkg
{
  // Calculate SHA256 sum of the specified memory buffer in binary mode. Issue
  // diagnostics and throw failed if anything goes wrong.
  //
  string
  sha256 (const common_options&, const char* buf, size_t n);

  // The same but for a stream (if ifdstream, open in binary mode).
  //
  string
  sha256 (const common_options&, istream&);

  // The same but for a file.
  //
  string
  sha256 (const common_options&, const path& file);
}

#endif // BPKG_CHECKSUM_HXX
