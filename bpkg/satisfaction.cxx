// file      : bpkg/satisfaction.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/satisfaction>

#include <bpkg/package-odb>
#include <bpkg/diagnostics>

using namespace std;
using namespace butl;

namespace bpkg
{
  bool
  satisfies (const version& v, const dependency_constraint& c)
  {
    using op = comparison;

    // Note that the constraint's version is always rhs (libfoo >= 1.2.3).
    //
    switch (c.operation)
    {
    case op::eq: return v == c.version;
    case op::lt: return v <  c.version;
    case op::gt: return v >  c.version;
    case op::le: return v <= c.version;
    case op::ge: return v >= c.version;
    }

    assert (false);
    return false;
  }

  bool
  satisfies (const dependency_constraint& l, const dependency_constraint& r)
  {
    using op = comparison;

    op lo (l.operation);
    op ro (r.operation);

    const version& lv (l.version);
    const version& rv (r.version);

    switch (lo)
    {
    case op::eq: // ==
      {
        return ro == op::eq && lv == rv;
      }
    case op::lt: // <
      {
        switch (ro)
        {
        case op::eq: return rv <  lv;
        case op::lt: return rv <= lv;
        case op::le: return rv <  lv;
        case op::gt:
        case op::ge: return false;
        }
      }
    case op::le: // <=
      {
        switch (ro)
        {
        case op::eq: return rv <= lv;
        case op::lt: return rv <  lv;
        case op::le: return rv <= lv;
        case op::gt:
        case op::ge: return false;
        }
      }
    case op::gt: // >
      {
        switch (ro)
        {
        case op::eq: return lv >  rv;
        case op::lt:
        case op::le: return false;
        case op::gt: return lv >= rv;
        case op::ge: return lv >  rv;
        }
      }
    case op::ge: // >=
      {
        switch (ro)
        {
        case op::eq: return lv >= rv;
        case op::lt:
        case op::le: return false;
        case op::gt: return lv >  rv;
        case op::ge: return lv >= rv;
        }
      }
    }

    assert (false);
    return false;
  }
}
