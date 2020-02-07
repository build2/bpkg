// file      : bpkg/checksum.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_CHECKSUM_HXX
#define BPKG_CHECKSUM_HXX

#include <libbutl/sha256.mxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/common-options.hxx>

namespace bpkg
{
  // Calculate SHA256 sum of the specified memory buffer in binary mode.
  //
  inline string
  sha256 (const char* buf, size_t n) {return butl::sha256 (buf, n).string ();}

  // The same but for a file. Issue diagnostics and throw failed if anything
  // goes wrong.
  //
  // Note that unlike the other overloads, this function runs the sha256
  // program underneath. The reason for this is that the program can be
  // optimized for the platform.
  //
  string
  sha256 (const common_options&, const path& file);
}

#endif // BPKG_CHECKSUM_HXX
