// file      : bpkg/openssl.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_OPENSSL_HXX
#define BPKG_OPENSSL_HXX

#include <butl/process>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/common-options.hxx>

namespace bpkg
{
  // Start the openssl process. Parameters in, out, err flags if the caller
  // wish to write to, or read from the process STDIN, STDOUT, STDERR streams.
  // If out and err are both true, then STDERR is redirected to STDOUT, and
  // they both can be read from in_ofd descriptor.
  //
  butl::process
  start_openssl (const common_options&,
                 const char* command,
                 const cstrings& options,
                 bool in = false,
                 bool out = false,
                 bool err = false);
}

#endif // BPKG_OPENSSL_HXX
