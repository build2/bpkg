// file      : bpkg/package.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace bpkg
{
  // available_package_id
  //
  inline available_package_id::
  available_package_id (package_name n, const bpkg::version& v)
      : name (move (n)),
        version (v)
  {
  }

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
