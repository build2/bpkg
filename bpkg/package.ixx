// file      : bpkg/package.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace bpkg
{
  inline bool available_package::
  has_dependency_constraint () const
  {
    for (const dependency_alternatives_ex& d: dependencies)
    {
      if (d.type == dependency_alternatives_type::constraint)
        return true;
    }

    return false;
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
