// file      : bpkg/satisfaction.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/satisfaction.hxx>

#undef NDEBUG
#include <cassert>

namespace bpkg
{
  static int
  main (int, char*[])
  {
    using vc = version_constraint;

    assert ( satisfies (vc ("[1.0   2.0]"),   vc ("[1.0+0 2.0]")));
    assert (!satisfies (vc ("[1.0   2.0]"),   vc ("[1.0+1 2.0]")));
    assert ( satisfies (vc ("[1.0+0 2.0]"),   vc ("[1.0   2.0]")));
    assert ( satisfies (vc ("[1.0+1 2.0]"),   vc ("[1.0   2.0]")));

    assert (!satisfies (vc ("[1.0+0 2.0]"),   vc ("(1.0   2.0]")));
    assert (!satisfies (vc ("[1.0+1 2.0]"),   vc ("(1.0   2.0]")));
    assert (!satisfies (vc ("(1.0+0 2.0]"),   vc ("(1.0   2.0]")));
    assert (!satisfies (vc ("(1.0+1 2.0]"),   vc ("(1.0   2.0]")));
    assert ( satisfies (vc ("(1.0+0 2.0]"),   vc ("[1.0   2.0]")));
    assert ( satisfies (vc ("(1.0+1 2.0]"),   vc ("[1.0   2.0]")));

    assert (!satisfies (vc ("[1.0   2.0+0]"), vc ("[1.0   2.0)")));
    assert (!satisfies (vc ("[1.0   2.0+1]"), vc ("[1.0   2.0)")));
    assert ( satisfies (vc ("[1.0   2.0+0)"), vc ("[1.0   2.0)")));
    assert (!satisfies (vc ("[1.0   2.0+1)"), vc ("[1.0   2.0)")));

    // Swap the above constraints.
    //
    assert (!satisfies (vc ("[1.0   2.0]"),   vc ("[1.0   2.0+0]")));
    assert (!satisfies (vc ("[1.0   2.0]"),   vc ("[1.0   2.0+1]")));
    assert ( satisfies (vc ("[1.0   2.0+0]"), vc ("[1.0   2.0]")));
    assert ( satisfies (vc ("[1.0   2.0+1]"), vc ("[1.0   2.0]")));

    assert ( satisfies (vc ("(1.0   2.0]"),   vc ("[1.0+0 2.0]")));
    assert ( satisfies (vc ("(1.0   2.0]"),   vc ("[1.0+1 2.0]")));
    assert ( satisfies (vc ("(1.0   2.0]"),   vc ("(1.0+0 2.0]")));
    assert ( satisfies (vc ("(1.0   2.0]"),   vc ("(1.0+1 2.0]")));
    assert (!satisfies (vc ("[1.0   2.0]"),   vc ("(1.0+0 2.0]")));
    assert (!satisfies (vc ("[1.0   2.0]"),   vc ("(1.0+1 2.0]")));

    assert ( satisfies (vc ("[1.0   2.0)"),   vc ("[1.0   2.0+0)")));
    assert ( satisfies (vc ("[1.0   2.0)"),   vc ("[1.0   2.0+1)")));
    assert (!satisfies (vc ("[1.0   2.0]"),   vc ("[1.0   2.0+0)")));
    assert (!satisfies (vc ("[1.0   2.0]"),   vc ("[1.0   2.0+1)")));

    assert (satisfies (vc ("^1.0.0"), vc ("^1.0.0")));

    return 0;
  }
}

int
main (int argc, char* argv[])
{
  return bpkg::main (argc, argv);
}
