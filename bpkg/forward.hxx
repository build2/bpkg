// file      : bpkg/forward.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_FORWARD_HXX
#define BPKG_FORWARD_HXX

#include <odb/sqlite/forward.hxx>

namespace bpkg
{
  using odb::sqlite::database;
  using odb::sqlite::transaction;

  // <bpkg/package.hxx>
  //
  class selected_package;
}

#endif // BPKG_FORWARD_HXX