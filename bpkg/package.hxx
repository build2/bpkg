// file      : bpkg/package.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PACKAGE_HXX
#define BPKG_PACKAGE_HXX

#include <map>
#include <set>
#include <ratio>
#include <chrono>
#include <type_traits> // static_assert

#include <odb/core.hxx>
#include <odb/nested-container.hxx>

#include <butl/timestamp>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#pragma db model version(3, 3, closed)

namespace bpkg
{
  // Compare two lazy pointers via the pointed-to object ids.
  //
  struct compare_lazy_ptr
  {
    template <typename P>
    bool
    operator() (const P& x, const P& y) const
    {
      return x.object_id () < y.object_id ();
    }
  };

  // path
  //
  using optional_string = optional<string>;
  using optional_path = optional<path>;
  using optional_dir_path = optional<dir_path>;

  #pragma db map type(path) as(string)  \
    to((?).string ()) from(bpkg::path (?))

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

  // As pointed out in butl/timestamp we will overflow in year 2262, but by
  // that time some larger basic type will be available for mapping.
  //
  #pragma db map type(timestamp) as(uint64_t)                 \
    to(std::chrono::duration_cast<std::chrono::nanoseconds> ( \
         (?).time_since_epoch ()).count ())                   \
    from(butl::timestamp (                                    \
      std::chrono::duration_cast<butl::timestamp::duration> ( \
        std::chrono::nanoseconds (?))))

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
    uint16_t revision;
    string upstream;
    optional<string> release;
  };
}

#include <libbpkg/manifest.hxx>

#include <bpkg/system-repository.hxx>

// Prevent assert() macro expansion in get/set expressions. This should
// appear after all #include directives since the assert() macro is
// redefined in each <assert.h> inclusion.
//
#ifdef ODB_COMPILER_HXX
#  undef assert
#  define assert assert
void assert (int);
#endif

namespace bpkg
{
  // version
  //
  // Sometimes we need to split the version into two parts: the part
  // that goes into the object id (epoch, canonical upstream, canonical
  // release, revision) and the original upstream and release. This is what
  // the canonical_version and upstream_version value types are for. Note that
  // upstream_version derives from version and uses it as storage. The idea
  // here is this: when we split the version, we often still want to have the
  // "whole" version object readily accessible and that's exactly what this
  // strange contraption is for. See available_package for an example
  // on how everything fits together.
  //
  //
  #pragma db value
  struct canonical_version
  {
    uint16_t epoch;
    string   canonical_upstream;
    string   canonical_release;
    uint16_t revision;

    // By default SQLite3 uses BINARY collation for TEXT columns. So while this
    // means we don't need to do anything special to make "absent" (~) and
    // specified canonical releases compare properly, better make it explicit
    // in case the Unicode Collation Algorithm (UCA, where '~' < 'a') becomes
    // the default.
    //
    #pragma db member(canonical_release) options("COLLATE BINARY")
  };

  #pragma db value transient
  struct upstream_version: version
  {
    #pragma db member(upstream_) virtual(string)                      \
      get(this.upstream)                                              \
      set(this = bpkg::version (0, std::move (?), std::string (), 0))

    #pragma db member(release_) virtual(optional_string)              \
      get(this.release)                                               \
      set(this = bpkg::version (                                      \
            0, std::move (this.upstream), std::move (?), 0))

    upstream_version () = default;
    upstream_version (version v): version (move (v)) {}
    upstream_version&
    operator= (version v) {version& b (*this); b = v; return *this;}

    void
    init (const canonical_version& cv, const upstream_version& uv)
    {
      *this = version (cv.epoch, uv.upstream, uv.release, cv.revision);
      assert (cv.canonical_upstream == canonical_upstream &&
              cv.canonical_release == canonical_release);
    }
  };

  #pragma db map type(version) as(_version)       \
    to(bpkg::_version{(?).epoch,                  \
                      (?).canonical_upstream,     \
                      (?).canonical_release,      \
                      (?).revision,               \
                      (?).upstream,               \
                      (?).release})               \
    from(bpkg::version ((?).epoch,                \
                        std::move ((?).upstream), \
                        std::move ((?).release),  \
                        (?).revision))

  using optional_version = optional<version>;
  using _optional_version = optional<_version>;

  #pragma db map type(optional_version) as(_optional_version) \
    to((?)                                                    \
       ? bpkg::_version{(?)->epoch,                           \
                        (?)->canonical_upstream,              \
                        (?)->canonical_release,               \
                        (?)->revision,                        \
                        (?)->upstream,                        \
                        (?)->release}                         \
       : bpkg::_optional_version ())                          \
    from((?)                                                  \
         ? bpkg::version ((?)->epoch,                         \
                          std::move ((?)->upstream),          \
                          std::move ((?)->release),           \
                          (?)->revision)                      \
         : bpkg::optional_version ())

  // repository_location
  //
  #pragma db map type(repository_location) as(string)     \
    to((?).string ()) from(bpkg::repository_location (?))


  // repository
  //
  #pragma db object pointer(shared_ptr) session
  class repository
  {
  public:
    // We use a weak pointer for prerequisite repositories because we
    // could have cycles. No cycles in complements, thought.
    //
    using complements_type =
      std::set<lazy_shared_ptr<repository>, compare_lazy_ptr>;
    using prerequisites_type =
      std::set<lazy_weak_ptr<repository>, compare_lazy_ptr>;

    string name; // Object id (canonical name).
    repository_location location;
    complements_type complements;
    prerequisites_type prerequisites;

    // Used to detect recursive fetching. Will probably be replaced
    // by the 'repositories' file timestamp or hashsum later.
    //
    #pragma db transient
    bool fetched = false;

  public:
    explicit
    repository (repository_location l): location (move (l))
    {
      name = location.canonical_name ();
    }

    // Database mapping.
    //
    #pragma db member(name) id

    #pragma db member(location)                                  \
      set(this.location = std::move (?);                         \
          assert (this.name == this.location.canonical_name ()))

    #pragma db member(complements) id_column("repository") \
      value_column("complement") value_not_null

    #pragma db member(prerequisites) id_column("repository") \
      value_column("prerequisite") value_not_null

  private:
    friend class odb::access;
    repository () = default;
  };

  #pragma db view object(repository) query(repository::name != "" && (?))
  struct repository_count
  {
    #pragma db column("count(*)")
    size_t result;

    operator size_t () const {return result;}
  };


  // package_location
  //
  #pragma db value
  struct package_location
  {
    using repository_type = bpkg::repository;

    lazy_shared_ptr<repository_type> repository;
    path location; // Relative to the repository.
  };

  // dependencies
  //
  #pragma db value(dependency_constraint) definition
  #pragma db value(dependency) definition
  #pragma db member(dependency::constraint) column("")
  #pragma db value(dependency_alternatives) definition

  using dependencies = vector<dependency_alternatives>;

  // Wildcard version. Satisfies any dependency constraint and is represented
  // as 0+0 (which is also the "stub version"; since a real version is always
  // greater than the stub version, we reuse it to signify a special case).
  //
  extern const version wildcard_version;

  // available_package
  //
  #pragma db value
  struct available_package_id
  {
    string name;
    canonical_version version;

    available_package_id () = default;
    available_package_id (string, const bpkg::version&);
  };

  bool
  operator< (const available_package_id&, const available_package_id&);

  #pragma db object pointer(shared_ptr) session
  class available_package
  {
  public:
    using version_type = bpkg::version;

    available_package_id id;
    upstream_version version;

    // List of repositories to which this package version belongs (yes,
    // in our world, it can be in multiple, unrelated repositories).
    //
    // Note that if the repository is the special root repository (its
    // location is empty), then this is a transient (or "fake") object
    // for an existing package archive or package directory. In this
    // case the location is the path to the archive/directory and to
    // determine which one it is, use file/dir_exists(). While on the
    // topic of fake available_package objects, when one is created for
    // a selected package (see make_available()), this list is left empty
    // with the thinking being that since the package is already in at
    // least fetched state, we shouldn't be needing its location.
    //
    vector<package_location> locations; //@@ Map?

    // Package manifest data.
    //
    using dependencies_type = bpkg::dependencies;

    dependencies_type dependencies;

    // Present for non-transient objects only.
    //
    optional<string> sha256sum;

  private:
    #pragma db transient
    mutable optional<version_type> system_version_;

  public:
    available_package (package_manifest&& m)
        : id (move (m.name), m.version),
          version (move (m.version)),
          dependencies (move (m.dependencies)),
          sha256sum (move (m.sha256sum)) {}

    // Create a stub available package with a fixed system version. This
    // constructor is only used to create transient/fake available packages
    // based on the system selected packages.
    //
    available_package (string n, version_type sysv)
        : id (move (n), wildcard_version),
          version (wildcard_version),
          system_version_ (sysv) {}

    bool
    stub () const {return version.compare (wildcard_version, true) == 0;}

    // Return package system version if one has been discovered. Note that
    // we do not implicitly assume a wildcard version.
    //
    const version_type*
    system_version () const
    {
      if (!system_version_)
      {
        if (const system_package* sp = system_repository.find (id.name))
        {
          // Only cache if it is authoritative.
          //
          if (sp->authoritative)
            system_version_ = sp->version;
          else
            return &sp->version;
        }
      }

      return system_version_ ? &*system_version_ : nullptr;
    }

    // As above but also return an indication if the version information is
    // authoritative.
    //
    pair<const version_type*, bool>
    system_version_authoritative () const
    {
      const system_package* sp (system_repository.find (id.name));

      if (!system_version_)
      {
        if (sp != nullptr)
        {
          // Only cache if it is authoritative.
          //
          if (sp->authoritative)
            system_version_ = sp->version;
          else
            return make_pair (&sp->version, false);
        }
      }

      return make_pair (system_version_ ?  &*system_version_ : nullptr,
                        sp != nullptr ? sp->authoritative : false);
    }

    // Database mapping.
    //
    #pragma db member(id) id column("")
    #pragma db member(version) set(this.version.init (this.id.version, (?)))
    #pragma db member(locations) id_column("") value_column("") \
      unordered value_not_null

    // dependencies
    //
    using _dependency_key = odb::nested_key<dependency_alternatives>;
    using _dependency_alternatives_type =
      std::map<_dependency_key, dependency>;

    #pragma db value(_dependency_key)
    #pragma db member(_dependency_key::outer) column("dependency_index")
    #pragma db member(_dependency_key::inner) column("index")

    #pragma db member(dependencies) id_column("") value_column("")
    #pragma db member(dependency_alternatives)                \
      virtual(_dependency_alternatives_type)                  \
      after(dependencies)                                     \
      get(odb::nested_get (this.dependencies))                \
      set(odb::nested_set (this.dependencies, std::move (?))) \
      id_column("") key_column("") value_column("dep_")

  private:
    friend class odb::access;
    available_package () = default;
  };

  #pragma db view object(available_package)
  struct available_package_count
  {
    #pragma db column("count(*)")
    size_t result;

    operator size_t () const {return result;}
  };

  // Only return packages that are in the specified repository or its
  // complements, recursively. While you could maybe come up with a
  // (barely comprehensible) view/query to achieve this, doing it on
  // the "client side" is definitely more straightforward.
  //
  vector<shared_ptr<available_package>>
  filter (const shared_ptr<repository>&, odb::result<available_package>&&);

  pair<shared_ptr<available_package>, shared_ptr<repository>>
  filter_one (const shared_ptr<repository>&, odb::result<available_package>&&);

  // package_state
  //
  enum class package_state
  {
    transient, // No longer or not yet in the database.
    broken,
    fetched,
    unpacked,
    configured
  };

  string
  to_string (package_state);

  package_state
  to_package_state (const string&); // May throw invalid_argument.

  inline ostream&
  operator<< (ostream& os, package_state s) {return os << to_string (s);}

  #pragma db map type(package_state) as(string) \
    to(to_string (?))                           \
    from(bpkg::to_package_state (?))

  // package_substate
  //
  enum class package_substate
  {
    none,
    system // System package; valid states: configured.
  };

  string
  to_string (package_substate);

  package_substate
  to_package_substate (const string&); // May throw invalid_argument.

  inline ostream&
  operator<< (ostream& os, package_substate s) {return os << to_string (s);}

  #pragma db map type(package_substate) as(string) \
    to(to_string (?))                              \
    from(bpkg::to_package_substate (?))

  // package
  //
  #pragma db object pointer(shared_ptr) session
  class selected_package
  {
  public:
    using version_type = bpkg::version;

    string name; // Object id.
    version_type version;
    package_state state;
    package_substate substate;

    // The hold flags indicate whether this package and/or version
    // should be retained in the configuration. A held package will
    // not be automatically removed. A held version will not be
    // automatically upgraded. Note also that the two flags are
    // orthogonal: we may want to keep a specific version of the
    // package as long as it has dependents.
    //
    bool hold_package;
    bool hold_version;

    // Repository from which this package came. Note that it is not
    // a pointer to the repository object because it could be wiped
    // out (e.g., as a result of rep-fetch). We call such packages
    // "orphans". While we can get a list of orphan's prerequisites
    // (by loading its manifest), we wouldn't know which repository
    // to use as a base to resolve them. As a result, an orphan that
    // is not already configured (and thus has all its prerequisites
    // resolved) is not very useful and can only be purged.
    //
    repository_location repository;

    // Path to the archive of this package, if any. If not absolute,
    // then it is relative to the configuration directory. The purge
    // flag indicates whether the archive should be removed when the
    // packaged is purged. If the archive is not present, it should
    // be false.
    //
    optional<path> archive;
    bool purge_archive;

    // Path to the source directory of this package, if any. If not
    // absolute, then it is relative to the configuration directory.
    // The purge flag indicates whether the directory should be
    // removed when the packaged is purged. If the source directory
    // is not present, it should be false.
    //
    optional<dir_path> src_root;
    bool purge_src;

    // Path to the output directory of this package, if any. It is
    // always relative to the configuration directory and currently
    // is always <name>-<version>. It is only set once the package
    // is configured and its main purse is to keep track of what
    // needs to be cleaned by the user before a broken package can
    // be purged. Note that it could be the same as out_root.
    //
    optional<dir_path> out_root;

    // A map of "effective" prerequisites (i.e., pointers to other selected
    // packages) to optional dependency constraint. Note that because it is a
    // single constraint, we don't support multiple dependencies on the same
    // package (e.g., two ranges of versions). See pkg_configure().
    //
    using prerequisites_type = std::map<lazy_shared_ptr<selected_package>,
                                        optional<dependency_constraint>,
                                        compare_lazy_ptr>;
    prerequisites_type prerequisites;

    bool
    system () const
    {
      // The system substate is only valid for the configured state.
      //
      assert (substate != package_substate::system ||
              state == package_state::configured);

      return substate == package_substate::system;
    }

    // Represent the wildcard version with the "*" string. Represent naturally
    // all other versions.
    //
    string
    version_string () const;

    // Database mapping.
    //
    #pragma db member(name) id

    #pragma db member(prerequisites) id_column("package") \
      key_column("prerequisite") value_column("") key_not_null

  private:
    friend class odb::access;
    selected_package () = default;
  };

  // Print the package name, version and the 'sys:' prefix for the system
  // substate. The wildcard version is represented with the "*" string.
  //
  ostream&
  operator<< (ostream&, const selected_package&);

  // certificate
  //
  // Information extracted from a repository X.509 certificate. The actual
  // certificate is stored on disk as .bpkg/certs/<fingerprint>.pem (we have
  // to store it as a file because that's the only way to pass it to openssl).
  //
  // If a repository is not authenticated (has no certificate/signature,
  // called unauth from now on), then we ask for the user's confirmation and
  // create a dummy certificate in order not to ask for the same confirmation
  // (for this repository) on next fetch. The problem is, there could be
  // multiple sections in such a repository and it would be annoying to
  // confirm all of them. So what we are going to do is create a dummy
  // certificate not for this specific repository location but for a
  // repository location only up to the version, so the name member will
  // contain the name prefix rather than the full name (just like a normal
  // certificate would). The fingerprint member for such a dummy certificate
  // contains the SHA256 checksum of this name. Members other then name and
  // fingerprint are meaningless for the dummy certificate.
  //
  #pragma db object pointer(shared_ptr) session
  class certificate
  {
  public:
    string fingerprint;  // Object id (note: SHA256 fingerprint).

    string name;         // CN component of Subject.
    string organization; // O component of Subject.
    string email;        // email: in Subject Alternative Name.

    timestamp start_date; // notBefore (UTC)
    timestamp end_date;   // notAfter  (UTC)

    bool
    dummy () const {return start_date == timestamp_unknown;}

    bool
    expired () const
    {
      assert (!dummy ());
      return timestamp::clock::now () > end_date;
    }

  public:
    certificate (string f,
                 string n,
                 string o,
                 string e,
                 timestamp sd,
                 timestamp ed)
        : fingerprint (move (f)),
          name (move (n)),
          organization (move (o)),
          email (move (e)),
          start_date (move (sd)),
          end_date (move (ed))
    {
    }

    // Create dummy certificate.
    //
    certificate (string f, string n)
        : fingerprint (move (f)),
          name (move (n)),
          start_date (timestamp_unknown),
          end_date (timestamp_unknown)
    {
    }

    // Database mapping.
    //
    #pragma db member(fingerprint) id

  private:
    friend class odb::access;
    certificate () = default;
  };

  // Note: prints all the certificate information on one line so mostly
  // useful for tracing.
  //
  ostream&
  operator<< (ostream&, const certificate&);

  // Return a list of packages that depend on this package along with
  // their constraints.
  //
  /*
  #pragma db view object(selected_package) \
    container(selected_package::prerequisites = pp inner: pp.key)
  struct package_dependent
  {
    #pragma db column(pp.id)
    string name;

    #pragma db column(pp.value)
    optional<dependency_constraint> constraint;
  };
  */

  // @@ Using raw container table since ODB doesn't support containers
  //    in views yet.
  //
  #pragma db view object(selected_package)               \
    table("selected_package_prerequisites" = "pp" inner: \
          "pp.prerequisite = " + selected_package::name)
  struct package_dependent
  {
    #pragma db column("pp.package")
    string name;

    #pragma db column("pp.")
    optional<dependency_constraint> constraint;
  };


  // Version comparison operators.
  //
  // They allow comparing objects that have epoch, canonical_upstream,
  // canonical_release, and revision data members. The idea is that this
  // works for both query members of types version and canonical_version
  // as well as for comparing canonical_version to version.
  //
  template <typename T1, typename T2>
  inline auto
  compare_version_eq (const T1& x, const T2& y, bool revision)
    -> decltype (x.epoch == y.epoch)
  {
    // Since we don't quite know what T1 and T2 are (and where the resulting
    // expression will run), let's not push our luck with something like
    // (!revision || x.revision == y.revision).
    //
    auto r (x.epoch == y.epoch &&
            x.canonical_upstream == y.canonical_upstream &&
            x.canonical_release == y.canonical_release);

    return revision
      ? r && x.revision == y.revision
      : r;
  }

  /*
  Currently unused (and probably should stay that way).

  template <typename T1, typename T2>
  inline auto
  operator== (const T1& x, const T2& y) -> decltype (x.epoch == y.epoch)
  {
  return compare_version_eq (x, y, true);
  }
  */

  template <typename T1, typename T2>
  inline auto
  compare_version_ne (const T1& x, const T2& y, bool revision)
    -> decltype (x.epoch == y.epoch)
  {
    auto r (x.epoch != y.epoch ||
            x.canonical_upstream != y.canonical_upstream ||
            x.canonical_release != y.canonical_release);

    return revision
      ? r || x.revision != y.revision
      : r;
  }

  template <typename T1, typename T2>
  inline auto
  operator!= (const T1& x, const T2& y) -> decltype (x.epoch != y.epoch)
  {
    return compare_version_ne (x, y, true);
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_lt (const T1& x, const T2& y, bool revision)
    -> decltype (x.epoch == y.epoch)
  {
    auto r (
      x.epoch < y.epoch ||
      (x.epoch == y.epoch && x.canonical_upstream < y.canonical_upstream) ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release < y.canonical_release));

    return revision
      ? r ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release == y.canonical_release && x.revision < y.revision)
      : r;
  }

  template <typename T1, typename T2>
  inline auto
  operator< (const T1& x, const T2& y) -> decltype (x.epoch < y.epoch)
  {
    return compare_version_lt (x, y, true);
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_le (const T1& x, const T2& y, bool revision)
    -> decltype (x.epoch == y.epoch)
  {
    auto r (
      x.epoch < y.epoch ||
      (x.epoch == y.epoch && x.canonical_upstream < y.canonical_upstream));

    return revision
      ? r ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release < y.canonical_release) ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release == y.canonical_release && x.revision <= y.revision)
      : r ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release <= y.canonical_release);
  }

  /*
  Currently unused (and probably should stay that way).

  template <typename T1, typename T2>
  inline auto
  operator<= (const T1& x, const T2& y) -> decltype (x.epoch <= y.epoch)
  {
    return compare_version_le (x, y, true);
  }
  */

  template <typename T1, typename T2>
  inline auto
  compare_version_gt (const T1& x, const T2& y, bool revision)
    -> decltype (x.epoch == y.epoch)
  {
    auto r (
      x.epoch > y.epoch ||
      (x.epoch == y.epoch && x.canonical_upstream > y.canonical_upstream) ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release > y.canonical_release));

    return revision
      ? r ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release == y.canonical_release && x.revision > y.revision)
      : r;
  }

  template <typename T1, typename T2>
  inline auto
  operator> (const T1& x, const T2& y) -> decltype (x.epoch > y.epoch)
  {
    return compare_version_gt (x, y, true);
  }

  template <typename T1, typename T2>
  inline auto
  compare_version_ge (const T1& x, const T2& y, bool revision)
    -> decltype (x.epoch == y.epoch)
  {
    auto r (
      x.epoch > y.epoch ||
      (x.epoch == y.epoch && x.canonical_upstream > y.canonical_upstream));

    return revision
      ? r ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release > y.canonical_release) ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release == y.canonical_release && x.revision >= y.revision)
      : r ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release >= y.canonical_release);
  }

  template <typename T1, typename T2>
  inline auto
  operator>= (const T1& x, const T2& y) -> decltype (x.epoch >= y.epoch)
  {
    return compare_version_ge (x, y, true);
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
      + x.revision + "DESC";
  }

  template <typename T>
  inline auto
  order_by_revision_desc (const T& x) -> //decltype ("ORDER BY" + x.epoch)
                                         decltype (x.revision == 0)
  {
    return "ORDER BY" + x.revision + "DESC";
  }
}

#include <bpkg/package.ixx>

#endif // BPKG_PACKAGE_HXX
