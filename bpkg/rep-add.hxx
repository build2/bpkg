// file      : bpkg/rep-add.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_REP_ADD_HXX
#define BPKG_REP_ADD_HXX

#include <libbpkg/manifest.hxx>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // database, repository
#include <bpkg/utility.hxx>

#include <bpkg/rep-add-options.hxx>

namespace bpkg
{
  int
  rep_add (const rep_add_options&, cli::scanner& args);

  // Create the new repository if it is not in the database yet or update its
  // location if it differs. Then add it as a complement to the root
  // repository if it is not already.
  //
  shared_ptr<repository>
  rep_add (database&, const repository_location&);
}

#endif // BPKG_REP_ADD_HXX
