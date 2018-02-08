// file      : bpkg/pkg-clean.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_CLEAN_HXX
#define BPKG_PKG_CLEAN_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/pkg-command.hxx>
#include <bpkg/pkg-clean-options.hxx>

namespace bpkg
{
  inline int
  pkg_clean (const pkg_clean_options& o, cli::scanner& args)
  {
    return pkg_command ("clean", o, "", args);
  }
}

#endif // BPKG_PKG_CLEAN_HXX
