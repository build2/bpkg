// file      : bpkg/package-common.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PACKAGE_COMMON_HXX
#define BPKG_PACKAGE_COMMON_HXX

#include <ratio>
#include <chrono>
#include <type_traits> // static_assert

#include <libbutl/timestamp.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

namespace bpkg
{
  // An image type that is used to map version to the database since
  // there is no way to modify individual components directly. We have
  // to define it before including <libbpkg/manifest.hxx> since some value
  // types that are defined there use version as their data members.
  //
  #pragma db value
  struct _version
  {
    uint16_t epoch;
    string canonical_upstream;
    string canonical_release;
    optional<uint16_t> revision;
    uint32_t iteration;
    string upstream;
    optional<string> release;

    // Work around MSVC 16.2 bug.
    //
    _version () = default;
    _version (uint16_t e,
              string cu, string cr,
              optional<uint16_t> rv, uint32_t i,
              string u, optional<string> rl)
        : epoch (e),
          canonical_upstream (move (cu)), canonical_release (move (cr)),
          revision (rv), iteration (i),
          upstream (move (u)), release (move (rl)) {}
  };
}

#include <libbpkg/manifest.hxx>

// Prevent assert() macro expansion in get/set expressions. This should
// appear after all #include directives since the assert() macro is
// redefined in each <assert.h> inclusion.
//
#ifdef ODB_COMPILER
#  undef assert
#  define assert assert
void assert (int);
#endif

namespace bpkg
{
  using optional_string = optional<string>;

  // path
  //
  using optional_path = optional<path>;
  using optional_dir_path = optional<dir_path>;

  // In some contexts it may denote directory, so lets preserve the trailing
  // slash, if present.
  //
  #pragma db map type(path) as(string)  \
    to((?).representation ()) from(bpkg::path (?))

  #pragma db map type(optional_path) as(bpkg::optional_string) \
    to((?) ? (?)->string () : bpkg::optional_string ())        \
    from((?) ? bpkg::path (*(?)) : bpkg::optional_path ())

  #pragma db map type(dir_path) as(string)  \
    to((?).string ()) from(bpkg::dir_path (?))

  #pragma db map type(optional_dir_path) as(bpkg::optional_string) \
    to((?) ? (?)->string () : bpkg::optional_string ())            \
    from((?) ? bpkg::dir_path (*(?)) : bpkg::optional_dir_path ())

  // timestamp
  //
  using butl::timestamp;
  using butl::timestamp_unknown;

  // Ensure that timestamp can be represented in nonoseconds without loss of
  // accuracy, so the following ODB mapping is adequate.
  //
  static_assert (
    std::ratio_greater_equal<timestamp::period,
                             std::chrono::nanoseconds::period>::value,
    "The following timestamp ODB mapping is invalid");

  // As pointed out in libbutl/timestamp.hxx we will overflow in year 2262, but
  // by that time some larger basic type will be available for mapping.
  //
  #pragma db map type(timestamp) as(uint64_t)                 \
    to(std::chrono::duration_cast<std::chrono::nanoseconds> ( \
         (?).time_since_epoch ()).count ())                   \
    from(butl::timestamp (                                    \
      std::chrono::duration_cast<butl::timestamp::duration> ( \
        std::chrono::nanoseconds (?))))

  using optional_uint64_t = optional<uint64_t>;   // Preserve uint64_t alias.
  using optional_timestamp = optional<timestamp>;

  #pragma db map type(optional_timestamp) as(bpkg::optional_uint64_t) \
    to((?)                                                            \
       ? std::chrono::duration_cast<std::chrono::nanoseconds> (       \
           (?)->time_since_epoch ()).count ()                         \
       : bpkg::optional_uint64_t ())                                  \
    from((?)                                                          \
         ? bpkg::timestamp (                                          \
             std::chrono::duration_cast<bpkg::timestamp::duration> (  \
               std::chrono::nanoseconds (*(?))))                      \
         : bpkg::optional_timestamp ())

  // repository_url
  //
  #pragma db value(repository_url) type("TEXT")

  // package_name
  //
  #pragma db value(package_name) type("TEXT") options("COLLATE NOCASE")

  // version
  //
  // Sometimes we need to split the version into two parts: the part
  // that goes into the object id (epoch, canonical upstream, canonical
  // release, revision) and the original upstream and release. This is what
  // the canonical_version and original_version value types are for. Note that
  // original_version derives from version and uses it as storage. The idea
  // here is this: when we split the version, we often still want to have the
  // "whole" version object readily accessible and that's exactly what this
  // strange contraption is for. See available_package for an example
  // on how everything fits together.
  //
  // Note that the object id cannot contain an optional member which is why we
  // make the revision type uint16_t and represent nullopt as zero. This
  // should be ok for package object ids referencing the package manifest
  // version values because an absent revision and zero revision mean the
  // same thing.
  //
  #pragma db value
  struct canonical_version
  {
    uint16_t epoch;
    string   canonical_upstream;
    string   canonical_release;
    uint16_t revision;
    uint32_t iteration;

    canonical_version () = default;

    explicit
    canonical_version (const version& v)
        : epoch (v.epoch),
          canonical_upstream (v.canonical_upstream),
          canonical_release (v.canonical_release),
          revision (v.effective_revision ()),
          iteration (v.iteration) {}

    // By default SQLite3 uses BINARY collation for TEXT columns. So while this
    // means we don't need to do anything special to make "absent" (~) and
    // specified canonical releases compare properly, better make it explicit
    // in case the Unicode Collation Algorithm (UCA, where '~' < 'a') becomes
    // the default.
    //
    #pragma db member(canonical_release) options("COLLATE BINARY")
  };

  #pragma db value transient
  struct original_version: version
  {
    #pragma db member(upstream_) virtual(string)                 \
      get(this.upstream)                                         \
      set(this = bpkg::version (                                 \
            0, std::move (?), std::string (), bpkg::nullopt, 0))

    #pragma db member(release_) virtual(optional_string)                    \
      get(this.release)                                                     \
      set(this = bpkg::version (                                            \
            0, std::move (this.upstream), std::move (?), bpkg::nullopt, 0))

    original_version () = default;
    original_version (version v): version (move (v)) {}
    original_version&
    operator= (version v) {version& b (*this); b = v; return *this;}

    void
    init (const canonical_version& cv, const original_version& uv)
    {
      // Note: revert the zero revision mapping (see above).
      //
      *this = version (cv.epoch,
                       uv.upstream,
                       uv.release,
                       (cv.revision != 0
                        ? optional<uint16_t> (cv.revision)
                        : nullopt),
                       cv.iteration);

      assert (cv.canonical_upstream == canonical_upstream &&
              cv.canonical_release == canonical_release);
    }
  };

  #pragma db map type(version) as(_version)       \
    to(bpkg::_version{(?).epoch,                  \
                      (?).canonical_upstream,     \
                      (?).canonical_release,      \
                      (?).revision,               \
                      (?).iteration,              \
                      (?).upstream,               \
                      (?).release})               \
    from(bpkg::version ((?).epoch,                \
                        std::move ((?).upstream), \
                        std::move ((?).release),  \
                        (?).revision,             \
                        (?).iteration))

  using optional_version = optional<version>;
  using _optional_version = optional<_version>;

  #pragma db map type(optional_version) as(_optional_version) \
    to((?)                                                    \
       ? bpkg::_version{(?)->epoch,                           \
                        (?)->canonical_upstream,              \
                        (?)->canonical_release,               \
                        (?)->revision,                        \
                        (?)->iteration,                       \
                        (?)->upstream,                        \
                        (?)->release}                         \
       : bpkg::_optional_version ())                          \
    from((?)                                                  \
         ? bpkg::version ((?)->epoch,                         \
                          std::move ((?)->upstream),          \
                          std::move ((?)->release),           \
                          (?)->revision,                      \
                          (?)->iteration)                     \
         : bpkg::optional_version ())

  // package_id
  //
  #pragma db value
  struct package_id
  {
    package_name name;
    canonical_version version;

    package_id () = default;
    package_id (package_name n, const bpkg::version& v)
        : name (move (n)), version (v) {}
  };

  // Version comparison operators and functions.
  //
  // They allow comparing objects that have epoch, canonical_upstream,
  // canonical_release, revision, and iteration data members. The idea is that
  // this works for both query members of types version and canonical_version.
  // Note, though, that the object revisions should be comparable (both
  // optional, numeric, etc), so to compare version to query member or
  // canonical_version you may need to explicitly convert the version object
  // to canonical_version.
  //
  // The compare_version_ref_*() functions compare the query members with the
  // canonical_version members, binding the latter by reference.
  //
  // Also note that if the comparison function ignores the revision, then it
  // also unconditionally ignores the iteration (that semantically extends the
  // revision).
  //
  template <typename T1, typename T2>
  inline auto
  compare_version_eq (const T1& x, const T2& y, bool revision, bool iteration)
    -> decltype (x.revision == y.revision)
  {
    assert (revision || !iteration); // !revision && iteration is meaningless.

    // Since we don't quite know what T1 and T2 are (and where the resulting
    // expression will run), let's not push our luck with something like
    // (!revision || x.revision == y.revision).
    //
    auto r (x.epoch == y.epoch                           &&
            x.canonical_upstream == y.canonical_upstream &&
            x.canonical_release == y.canonical_release);

    return !revision
      ? r
      : !iteration
        ? r && x.revision == y.revision
        : r && x.revision == y.revision && x.iteration == y.iteration;
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_ref_eq (const T1& x,
                          const T2& y,
                          bool revision,
                          bool iteration)
    -> decltype (x.revision == y.revision)
  {
    assert (revision || !iteration); // !revision && iteration is meaningless.

    using q = decltype (x.revision == y.revision);

    auto r (x.epoch == q::_ref (y.epoch)                           &&
            x.canonical_upstream == q::_ref (y.canonical_upstream) &&
            x.canonical_release == q::_ref (y.canonical_release));

    return !revision
      ? r
      : !iteration
        ? r && x.revision == q::_ref (y.revision)
        : r                                  &&
          x.revision == q::_ref (y.revision) &&
          x.iteration == q::_ref (y.iteration);
  }

  /*
  Currently unused (and probably should stay that way).

  template <typename T1, typename T2>
  inline auto
  operator== (const T1& x, const T2& y) -> decltype (x.revision == y.revision)
  {
  return compare_version_eq (x, y, true);
  }
  */

  template <typename T1, typename T2>
  inline auto
  compare_version_ne (const T1& x, const T2& y, bool revision, bool iteration)
    -> decltype (x.revision == y.revision)
  {
    assert (revision || !iteration); // !revision && iteration is meaningless.

    auto r (x.epoch != y.epoch                           ||
            x.canonical_upstream != y.canonical_upstream ||
            x.canonical_release != y.canonical_release);

    return !revision
      ? r
      : !iteration
        ? r || x.revision != y.revision
        : r || x.revision != y.revision || x.iteration != y.iteration;
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_ref_ne (const T1& x,
                          const T2& y,
                          bool revision,
                          bool iteration)
    -> decltype (x.revision == y.revision)
  {
    assert (revision || !iteration); // !revision && iteration is meaningless.

    using q = decltype (x.revision == y.revision);

    auto r (x.epoch != q::_ref (y.epoch)                           ||
            x.canonical_upstream != q::_ref (y.canonical_upstream) ||
            x.canonical_release != q::_ref (y.canonical_release));

    return !revision
      ? r
      : !iteration
        ? r || x.revision != q::_ref (y.revision)
        : r                                  ||
          x.revision != q::_ref (y.revision) ||
          x.iteration != q::_ref (y.iteration);
  }

  template <typename T1, typename T2>
  inline auto
  operator!= (const T1& x, const T2& y) -> decltype (x.revision != y.revision)
  {
    return compare_version_ne (x, y, true, true);
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_lt (const T1& x, const T2& y, bool revision, bool iteration)
    -> decltype (x.revision == y.revision)
  {
    assert (revision || !iteration); // !revision && iteration is meaningless.

    auto r (x.epoch < y.epoch ||
            (x.epoch == y.epoch &&
             (x.canonical_upstream < y.canonical_upstream ||
              (x.canonical_upstream == y.canonical_upstream &&
               x.canonical_release < y.canonical_release))));

    if (revision)
    {
      r = r || (x.epoch == y.epoch                           &&
                x.canonical_upstream == y.canonical_upstream &&
                x.canonical_release == y.canonical_release   &&
                x.revision < y.revision);

      if (iteration)
        r = r || (x.epoch == y.epoch                           &&
                  x.canonical_upstream == y.canonical_upstream &&
                  x.canonical_release == y.canonical_release   &&
                  x.revision == y.revision                     &&
                  x.iteration < y.iteration);
    }

    return r;
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_ref_lt (const T1& x,
                          const T2& y,
                          bool revision,
                          bool iteration)
    -> decltype (x.revision == y.revision)
  {
    assert (revision || !iteration); // !revision && iteration is meaningless.

    using q = decltype (x.revision == y.revision);

    auto r (x.epoch < q::_ref (y.epoch) ||
            (x.epoch == q::_ref (y.epoch) &&
             (x.canonical_upstream < q::_ref (y.canonical_upstream) ||
              (x.canonical_upstream == q::_ref (y.canonical_upstream) &&
               x.canonical_release < q::_ref (y.canonical_release)))));

    if (revision)
    {
      r = r || (x.epoch == q::_ref (y.epoch)                           &&
                x.canonical_upstream == q::_ref (y.canonical_upstream) &&
                x.canonical_release == q::_ref (y.canonical_release)   &&
                x.revision < q::_ref (y.revision));

      if (iteration)
        r = r || (x.epoch == q::_ref (y.epoch)                           &&
                  x.canonical_upstream == q::_ref (y.canonical_upstream) &&
                  x.canonical_release == q::_ref (y.canonical_release)   &&
                  x.revision == q::_ref (y.revision)                     &&
                  x.iteration < q::_ref (y.iteration));
    }

    return r;
  }

  template <typename T1, typename T2>
  inline auto
  operator< (const T1& x, const T2& y) -> decltype (x.revision < y.revision)
  {
    return compare_version_lt (x, y, true, true);
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_le (const T1& x, const T2& y, bool revision, bool iteration)
    -> decltype (x.revision == y.revision)
  {
    assert (revision || !iteration); // !revision && iteration is meaningless.

    auto r (
      x.epoch < y.epoch ||
      (x.epoch == y.epoch && x.canonical_upstream < y.canonical_upstream));

    if (!revision)
    {
      r = r || (x.epoch == y.epoch                           &&
                x.canonical_upstream == y.canonical_upstream &&
                x.canonical_release <= y.canonical_release);
    }
    else
    {
      r = r || (x.epoch == y.epoch                           &&
                x.canonical_upstream == y.canonical_upstream &&
                x.canonical_release < y.canonical_release);

      if (!iteration)
        r = r || (x.epoch == y.epoch                           &&
                  x.canonical_upstream == y.canonical_upstream &&
                  x.canonical_release == y.canonical_release   &&
                  x.revision <= y.revision);
      else
        r = r || (x.epoch == y.epoch                           &&
                  x.canonical_upstream == y.canonical_upstream &&
                  x.canonical_release == y.canonical_release   &&
                  (x.revision < y.revision ||
                   (x.revision == y.revision && x.iteration <= y.iteration)));
    }

    return r;
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_ref_le (const T1& x,
                          const T2& y,
                          bool revision,
                          bool iteration)
    -> decltype (x.revision == y.revision)
  {
    assert (revision || !iteration); // !revision && iteration is meaningless.

    using q = decltype (x.revision == y.revision);

    auto r (
      x.epoch < q::_ref (y.epoch) ||
      (x.epoch == q::_ref (y.epoch) &&
       x.canonical_upstream < q::_ref (y.canonical_upstream)));

    if (!revision)
    {
      r = r || (x.epoch == q::_ref (y.epoch)                           &&
                x.canonical_upstream == q::_ref (y.canonical_upstream) &&
                x.canonical_release <= q::_ref (y.canonical_release));
    }
    else
    {
      r = r || (x.epoch == q::_ref (y.epoch)                           &&
                x.canonical_upstream == q::_ref (y.canonical_upstream) &&
                x.canonical_release < q::_ref (y.canonical_release));

      if (!iteration)
        r = r || (x.epoch == q::_ref (y.epoch)                           &&
                  x.canonical_upstream == q::_ref (y.canonical_upstream) &&
                  x.canonical_release == q::_ref (y.canonical_release)   &&
                  x.revision <= q::_ref (y.revision));
      else
        r = r || (x.epoch == q::_ref (y.epoch)                           &&
                  x.canonical_upstream == q::_ref (y.canonical_upstream) &&
                  x.canonical_release == q::_ref (y.canonical_release)   &&
                  (x.revision < q::_ref (y.revision) ||
                   (x.revision == q::_ref (y.revision) &&
                    x.iteration <= q::_ref (y.iteration))));
    }

    return r;
  }

  /*
  Currently unused (and probably should stay that way).

  template <typename T1, typename T2>
  inline auto
  operator<= (const T1& x, const T2& y) -> decltype (x.revision <= y.revision)
  {
    return compare_version_le (x, y, true);
  }
  */

  template <typename T1, typename T2>
  inline auto
  compare_version_gt (const T1& x, const T2& y, bool revision, bool iteration)
    -> decltype (x.revision == y.revision)
  {
    assert (revision || !iteration); // !revision && iteration is meaningless.

    auto r (
      x.epoch > y.epoch ||
      (x.epoch == y.epoch &&
       (x.canonical_upstream > y.canonical_upstream ||
        (x.canonical_upstream == y.canonical_upstream &&
         x.canonical_release > y.canonical_release))));

    if (revision)
    {
      r = r || (x.epoch == y.epoch                           &&
                x.canonical_upstream == y.canonical_upstream &&
                x.canonical_release == y.canonical_release   &&
                x.revision > y.revision);

      if (iteration)
        r = r || (x.epoch == y.epoch                           &&
                  x.canonical_upstream == y.canonical_upstream &&
                  x.canonical_release == y.canonical_release   &&
                  x.revision == y.revision                     &&
                  x.iteration > y.iteration);
    }

    return r;
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_ref_gt (const T1& x,
                          const T2& y,
                          bool revision,
                          bool iteration)
    -> decltype (x.revision == y.revision)
  {
    assert (revision || !iteration); // !revision && iteration is meaningless.

    using q = decltype (x.revision == y.revision);

    auto r (
      x.epoch > q::_ref (y.epoch) ||
      (x.epoch == q::_ref (y.epoch) &&
       (x.canonical_upstream > q::_ref (y.canonical_upstream) ||
        (x.canonical_upstream == q::_ref (y.canonical_upstream) &&
         x.canonical_release > q::_ref (y.canonical_release)))));

    if (revision)
    {
      r = r || (x.epoch == q::_ref (y.epoch)                           &&
                x.canonical_upstream == q::_ref (y.canonical_upstream) &&
                x.canonical_release == q::_ref (y.canonical_release)   &&
                x.revision > q::_ref (y.revision));

      if (iteration)
        r = r || (x.epoch == q::_ref (y.epoch)                           &&
                  x.canonical_upstream == q::_ref (y.canonical_upstream) &&
                  x.canonical_release == q::_ref (y.canonical_release)   &&
                  x.revision == q::_ref (y.revision)                     &&
                  x.iteration > q::_ref (y.iteration));
    }

    return r;
  }

  template <typename T1, typename T2>
  inline auto
  operator> (const T1& x, const T2& y) -> decltype (x.revision > y.revision)
  {
    return compare_version_gt (x, y, true, true);
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_ge (const T1& x, const T2& y, bool revision, bool iteration)
    -> decltype (x.revision == y.revision)
  {
    assert (revision || !iteration); // !revision && iteration is meaningless.

    auto r (
      x.epoch > y.epoch ||
      (x.epoch == y.epoch && x.canonical_upstream > y.canonical_upstream));

    if (!revision)
    {
      r = r || (x.epoch == y.epoch                           &&
                x.canonical_upstream == y.canonical_upstream &&
                x.canonical_release >= y.canonical_release);
    }
    else
    {
      r = r || (x.epoch == y.epoch                           &&
                x.canonical_upstream == y.canonical_upstream &&
                x.canonical_release > y.canonical_release);

      if (!iteration)
        r = r || (x.epoch == y.epoch                           &&
                  x.canonical_upstream == y.canonical_upstream &&
                  x.canonical_release == y.canonical_release   &&
                  x.revision >= y.revision);
      else
        r = r || (x.epoch == y.epoch                           &&
                  x.canonical_upstream == y.canonical_upstream &&
                  x.canonical_release == y.canonical_release   &&
                  (x.revision > y.revision ||
                   (x.revision == y.revision && x.iteration >= y.iteration)));
    }

    return r;
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_ref_ge (const T1& x,
                          const T2& y,
                          bool revision,
                          bool iteration)
    -> decltype (x.revision == y.revision)
  {
    assert (revision || !iteration); // !revision && iteration is meaningless.

    using q = decltype (x.revision == y.revision);

    auto r (x.epoch > q::_ref (y.epoch) ||
            (x.epoch == q::_ref (y.epoch) &&
             x.canonical_upstream > q::_ref (y.canonical_upstream)));

    if (!revision)
    {
      r = r || (x.epoch == q::_ref (y.epoch)                           &&
                x.canonical_upstream == q::_ref (y.canonical_upstream) &&
                x.canonical_release >= q::_ref (y.canonical_release));
    }
    else
    {
      r = r || (x.epoch == q::_ref (y.epoch)                           &&
                x.canonical_upstream == q::_ref (y.canonical_upstream) &&
                x.canonical_release > q::_ref (y.canonical_release));

      if (!iteration)
        r = r || (x.epoch == q::_ref (y.epoch)                           &&
                  x.canonical_upstream == q::_ref (y.canonical_upstream) &&
                  x.canonical_release == q::_ref (y.canonical_release)   &&
                  x.revision >= q::_ref (y.revision));
      else
        r = r || (x.epoch == q::_ref (y.epoch)                           &&
                  x.canonical_upstream == q::_ref (y.canonical_upstream) &&
                  x.canonical_release == q::_ref (y.canonical_release)   &&
                  (x.revision > q::_ref (y.revision) ||
                   (x.revision == q::_ref (y.revision) &&
                    x.iteration >= q::_ref (y.iteration))));
    }

    return r;
  }

  template <typename T1, typename T2>
  inline auto
  operator>= (const T1& x, const T2& y) -> decltype (x.revision >= y.revision)
  {
    return compare_version_ge (x, y, true, true);
  }

  template <typename T>
  inline auto
  order_by_version_desc (const T& x) -> //decltype ("ORDER BY" + x.epoch)
                                        decltype (x.epoch == 0)
  {
    return "ORDER BY"
      + x.epoch + "DESC,"
      + x.canonical_upstream + "DESC,"
      + x.canonical_release + "DESC,"
      + x.revision + "DESC,"
      + x.iteration + "DESC";
  }
/*
  Currently unused (and probably should stay that way).

  template <typename T>
  inline auto
  order_by_revision_desc (const T& x) -> //decltype ("ORDER BY" + x.epoch)
                                         decltype (x.revision == 0)
  {
    return "ORDER BY" + x.revision + "DESC," + x.iteration + "DESC";
  }
*/

  inline bool
  operator< (const package_id& x, const package_id& y)
  {
    int r (x.name.compare (y.name));
    return r != 0 ? r < 0 : x.version < y.version;
  }
}

#endif // BPKG_PACKAGE_COMMON_HXX
