// file      : bpkg/drop.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/drop>

#include <iostream>   // cout

#include <butl/utility> // reverse_iterate()

#include <bpkg/types>
#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/utility>
#include <bpkg/database>
#include <bpkg/diagnostics>
#include <bpkg/satisfaction>
#include <bpkg/manifest-utility>

#include <bpkg/common-options>

#include <bpkg/pkg-purge>
#include <bpkg/pkg-disfigure>

using namespace std;
using namespace butl;

namespace bpkg
{
  void
  drop (const drop_options& o, cli::scanner& args)
  {
    tracer trace ("drop");

    const dir_path& c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    if (!args.more ())
      fail << "package name argument expected" <<
        info << "run 'bpkg help drop' for more information";

    database db (open (c, trace));

    // Note that the session spans all our transactions. The idea here is
    // that selected_package objects in the satisfied_packages list below
    // will be cached in this session. When subsequent transactions modify
    // any of these objects, they will modify the cached instance, which
    // means our list will always "see" their updated state.
    //
    session s;
  }
}
