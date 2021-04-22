// file      : bpkg/pointer-traits.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_POINTER_TRAITS_HXX
#define BPKG_POINTER_TRAITS_HXX

#include <bpkg/types.hxx>

#include <odb/pointer-traits.hxx>

namespace odb
{
  template <typename T>
  class pointer_traits<bpkg::lazy_shared_ptr<T>>
  {
  public:
    static const pointer_kind kind = pk_shared;
    static const bool lazy = true;

    typedef T element_type;
    typedef bpkg::lazy_shared_ptr<element_type> pointer_type;
    typedef bpkg::shared_ptr<element_type> eager_pointer_type;

    static bool
    null_ptr (const pointer_type& p)
    {
      return !p;
    }

    template <class O = T>
    static typename object_traits<O>::id_type
    object_id (const pointer_type& p)
    {
      return p.template object_id<O> ();
    }
  };

  template <typename T>
  class pointer_traits<bpkg::lazy_weak_ptr<T>>
  {
  public:
    static const pointer_kind kind = pk_weak;
    static const bool lazy = true;

    typedef T element_type;
    typedef bpkg::lazy_weak_ptr<element_type> pointer_type;
    typedef bpkg::lazy_shared_ptr<element_type> strong_pointer_type;
    typedef bpkg::weak_ptr<element_type> eager_pointer_type;

    static strong_pointer_type
    lock (const pointer_type& p)
    {
      return p.lock ();
    }
  };
}

#endif // BPKG_POINTER_TRAITS_HXX
