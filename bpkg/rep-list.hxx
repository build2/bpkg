// file      : bpkg/rep-list.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_REP_LIST_HXX
#define BPKG_REP_LIST_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/rep-list-options.hxx>

namespace bpkg
{
  int
  rep_list (const rep_list_options&, cli::scanner& args);
}

#endif // BPKG_REP_LIST_HXX
