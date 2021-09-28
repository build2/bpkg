// file      : bpkg/wrapper-traits.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_WRAPPER_TRAITS_HXX
#define BPKG_WRAPPER_TRAITS_HXX

#include <libbutl/optional.hxx>

#include <odb/wrapper-traits.hxx>

namespace odb
{
  template <typename T>
  class wrapper_traits<butl::optional<T>>
  {
  public:
    typedef T wrapped_type;
    typedef butl::optional<T> wrapper_type;

    // T can be const.
    //
    typedef
    typename odb::details::meta::remove_const<T>::result
    unrestricted_wrapped_type;

    static const bool null_handler = true;
    static const bool null_default = true;

    static bool
    get_null (const wrapper_type& o)
    {
      return !o;
    }

    static void
    set_null (wrapper_type& o)
    {
      o = wrapper_type ();
    }

    static const wrapped_type&
    get_ref (const wrapper_type& o)
    {
      return *o;
    }

    static unrestricted_wrapped_type&
    set_ref (wrapper_type& o)
    {
      if (!o)
        o = unrestricted_wrapped_type ();

      return const_cast<unrestricted_wrapped_type&> (*o);
    }
  };
}

#endif // BPKG_WRAPPER_TRAITS_HXX
