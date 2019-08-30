// file      : bpkg/satisfaction.test.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/satisfaction.hxx>

namespace bpkg
{
  static int
  main (int, char*[])
  {
    using dc = dependency_constraint;

    assert ( satisfies (dc ("[1.0   2.0]"),   dc ("[1.0+0 2.0]")));
    assert (!satisfies (dc ("[1.0   2.0]"),   dc ("[1.0+1 2.0]")));
    assert ( satisfies (dc ("[1.0+0 2.0]"),   dc ("[1.0   2.0]")));
    assert ( satisfies (dc ("[1.0+1 2.0]"),   dc ("[1.0   2.0]")));

    assert (!satisfies (dc ("[1.0+0 2.0]"),   dc ("(1.0   2.0]")));
    assert (!satisfies (dc ("[1.0+1 2.0]"),   dc ("(1.0   2.0]")));
    assert (!satisfies (dc ("(1.0+0 2.0]"),   dc ("(1.0   2.0]")));
    assert (!satisfies (dc ("(1.0+1 2.0]"),   dc ("(1.0   2.0]")));
    assert ( satisfies (dc ("(1.0+0 2.0]"),   dc ("[1.0   2.0]")));
    assert ( satisfies (dc ("(1.0+1 2.0]"),   dc ("[1.0   2.0]")));

    assert (!satisfies (dc ("[1.0   2.0+0]"), dc ("[1.0   2.0)")));
    assert (!satisfies (dc ("[1.0   2.0+1]"), dc ("[1.0   2.0)")));
    assert ( satisfies (dc ("[1.0   2.0+0)"), dc ("[1.0   2.0)")));
    assert (!satisfies (dc ("[1.0   2.0+1)"), dc ("[1.0   2.0)")));

    // Swap the above constraints.
    //
    assert (!satisfies (dc ("[1.0   2.0]"),   dc ("[1.0   2.0+0]")));
    assert (!satisfies (dc ("[1.0   2.0]"),   dc ("[1.0   2.0+1]")));
    assert ( satisfies (dc ("[1.0   2.0+0]"), dc ("[1.0   2.0]")));
    assert ( satisfies (dc ("[1.0   2.0+1]"), dc ("[1.0   2.0]")));

    assert ( satisfies (dc ("(1.0   2.0]"),   dc ("[1.0+0 2.0]")));
    assert ( satisfies (dc ("(1.0   2.0]"),   dc ("[1.0+1 2.0]")));
    assert ( satisfies (dc ("(1.0   2.0]"),   dc ("(1.0+0 2.0]")));
    assert ( satisfies (dc ("(1.0   2.0]"),   dc ("(1.0+1 2.0]")));
    assert (!satisfies (dc ("[1.0   2.0]"),   dc ("(1.0+0 2.0]")));
    assert (!satisfies (dc ("[1.0   2.0]"),   dc ("(1.0+1 2.0]")));

    assert ( satisfies (dc ("[1.0   2.0)"),   dc ("[1.0   2.0+0)")));
    assert ( satisfies (dc ("[1.0   2.0)"),   dc ("[1.0   2.0+1)")));
    assert (!satisfies (dc ("[1.0   2.0]"),   dc ("[1.0   2.0+0)")));
    assert (!satisfies (dc ("[1.0   2.0]"),   dc ("[1.0   2.0+1)")));

    return 0;
  }
}

int
main (int argc, char* argv[])
{
  return bpkg::main (argc, argv);
}
