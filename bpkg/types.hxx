// file      : bpkg/types.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_TYPES_HXX
#define BPKG_TYPES_HXX

#include <vector>
#include <string>
#include <memory>        // unique_ptr, shared_ptr
#include <utility>       // pair
#include <cstddef>       // size_t, nullptr_t
#include <cstdint>       // uint{8,16,32,64}_t
#include <istream>
#include <ostream>
#include <functional>    // function, reference_wrapper

#include <ios>           // ios_base::failure
#include <exception>     // exception
#include <stdexcept>     // logic_error, invalid_argument, runtime_error
#include <system_error>

#include <odb/lazy-ptr.hxx>

#include <libbutl/url.mxx>
#include <libbutl/path.mxx>
#include <libbutl/uuid.hxx>
#include <libbutl/uuid-io.hxx>
#include <libbutl/process.mxx>
#include <libbutl/utility.mxx>         // icase_compare_string,
                                       // compare_reference_target
#include <libbutl/optional.mxx>
#include <libbutl/fdstream.mxx>
#include <libbutl/small-vector.mxx>
#include <libbutl/default-options.mxx>

namespace bpkg
{
  // Commonly-used types.
  //
  using std::uint8_t;
  using std::uint16_t;
  using std::uint32_t;
  using std::uint64_t;

  using std::size_t;
  using std::nullptr_t;

  using std::pair;
  using std::string;
  using std::function;
  using std::reference_wrapper;

  using std::unique_ptr;
  using std::shared_ptr;
  using std::weak_ptr;

  using std::vector;
  using butl::small_vector; // <libbutl/small-vector.mxx>

  using strings = vector<string>;
  using cstrings = vector<const char*>;

  using std::istream;
  using std::ostream;

  // Exceptions. While <exception> is included, there is no using for
  // std::exception -- use qualified.
  //
  using std::logic_error;
  using std::invalid_argument;
  using std::runtime_error;
  using std::system_error;
  using io_error = std::ios_base::failure;

  // <libbutl/utility.mxx>
  //
  using butl::icase_compare_string;
  using butl::compare_reference_target;

  // <libbutl/optional.mxx>
  //
  using butl::optional;
  using butl::nullopt;

  // <libbutl/path.mxx>
  //
  using butl::path;
  using butl::dir_path;
  using butl::basic_path;
  using butl::invalid_path;

  // <libbutl/uuid.mxx>
  //
  using butl::uuid;

  using butl::path_cast;

  using paths = std::vector<path>;
  using dir_paths = std::vector<dir_path>;

  // <libbutl/url.mxx>
  //
  using butl::url;

  // <libbutl/process.mxx>
  //
  using butl::process;
  using butl::process_env;
  using butl::process_path;
  using butl::process_exit;
  using butl::process_error;

  // <libbutl/fdstream.mxx>
  //
  using butl::auto_fd;
  using butl::nullfd;
  using butl::fdpipe;
  using butl::ifdstream;
  using butl::ofdstream;
  using butl::fdstream_mode;

  // <libbutl/default-options.mxx>
  //
  using butl::default_options_files;
  using butl::default_options_entry;
  using butl::default_options;

  // Derive from ODB smart pointers to return derived database (note that the
  // database() functions are defined in database.hxx).
  //
  class database;

  template <class T>
  class lazy_shared_ptr: public odb::lazy_shared_ptr<T>
  {
  public:
    using base_type = odb::lazy_shared_ptr<T>;

    using base_type::base_type;

    explicit
    lazy_shared_ptr (base_type&& p): base_type (move (p)) {}

    lazy_shared_ptr () = default;

    bpkg::database&
    database () const;
  };

  template <class T>
  class lazy_weak_ptr: public odb::lazy_weak_ptr<T>
  {
  public:
    using base_type = odb::lazy_weak_ptr<T>;

    using base_type::base_type;

    bpkg::database&
    database () const;

    lazy_shared_ptr<T>
    lock () const
    {
      return lazy_shared_ptr<T> (base_type::lock ());
    }
  };

  struct compare_lazy_ptr
  {
    template <typename P>
    bool
    operator() (const P& x, const P& y) const
    {
      // See operator==(database, database).
      //
      return x.object_id () != y.object_id ()
             ? (x.object_id () < y.object_id ())
             : (&static_cast<typename P::base_type> (x).database () <
                &static_cast<typename P::base_type> (y).database ());
    }
  };

  // Compare two lazy pointers via the pointed-to object ids.
  //
  struct compare_lazy_ptr_id
  {
    template <typename P>
    bool
    operator() (const P& x, const P& y) const
    {
      // Note: ignoring database is intentional.
      //
      return x.object_id () < y.object_id ();
    }
  };
}

// In order to be found (via ADL) these have to be either in std:: or in
// butl::. The latter is bad idea since libbutl includes the default
// implementation.
//
namespace std
{
  // Custom path printing (canonicalized, with trailing slash for directories).
  //
  inline ostream&
  operator<< (ostream& os, const ::butl::path& p)
  {
    string r (p.representation ());
    ::butl::path::traits_type::canonicalize (r);
    return os << r;
  }
}

#endif // BPKG_TYPES_HXX
