// file      : bpkg/package.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace bpkg
{
  template <typename T>
  inline bool
  has_buildfile_clause (const vector<T>& ds)
  {
    for (const dependency_alternatives& das: ds)
    {
      for (const dependency_alternative& da: das)
      {
        // Note: the accept clause cannot be present if the prefer clause is
        // absent.
        //
        if (da.enable || da.reflect || da.prefer || da.require)
          return true;
      }
    }

    return false;
  }
}
