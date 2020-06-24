// file      : bpkg/package.hxx -*- C++ -*-
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

#include <libbutl/timestamp.mxx>

#include <libbpkg/package-name.hxx>

#include <bpkg/types.hxx>
#include <bpkg/forward.hxx> // transaction
#include <bpkg/utility.hxx>

#include <bpkg/diagnostics.hxx>

// Used by the data migration entries.
//
#define DB_SCHEMA_VERSION_BASE 5

#pragma db model version(DB_SCHEMA_VERSION_BASE, 7, closed)

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

  // As pointed out in libbutl/timestamp.mxx we will overflow in year 2262, but
  // by that time some larger basic type will be available for mapping.
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
    optional<uint16_t> revision;
    uint32_t iteration;
    string upstream;
    optional<string> release;

    // @@ TMP: work around MSVC 16.2 bug.
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

#include <bpkg/system-repository.hxx>

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
  struct upstream_version: version
  {
    #pragma db member(upstream_) virtual(string)                 \
      get(this.upstream)                                         \
      set(this = bpkg::version (                                 \
            0, std::move (?), std::string (), bpkg::nullopt, 0))

    #pragma db member(release_) virtual(optional_string)                    \
      get(this.release)                                                     \
      set(this = bpkg::version (                                            \
            0, std::move (this.upstream), std::move (?), bpkg::nullopt, 0))

    upstream_version () = default;
    upstream_version (version v): version (move (v)) {}
    upstream_version&
    operator= (version v) {version& b (*this); b = v; return *this;}

    void
    init (const canonical_version& cv, const upstream_version& uv)
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

  // repository_location
  //
  #pragma db value
  struct _repository_location
  {
    repository_url  url;
    repository_type type;

    // @@ TMP: work around MSVC 16.2 bug.
    //
    _repository_location () = default;
    _repository_location (repository_url u, repository_type t)
        : url (move (u)), type (t) {}
  };

  #pragma db map type(repository_url) as(string)                            \
    to((?).string ())                                                       \
    from((?).empty () ? bpkg::repository_url () : bpkg::repository_url (?))

  #pragma db map type(repository_type) as(string) \
    to(to_string (?))                             \
    from(bpkg::to_repository_type (?))

  // Note that the type() call fails for an empty repository location.
  //
  #pragma db map type(repository_location) as(_repository_location) \
    to(bpkg::_repository_location {(?).url (),                      \
                                   (?).empty ()                     \
                                   ? bpkg::repository_type::pkg     \
                                   : (?).type ()})                  \
    from(bpkg::repository_location (std::move ((?).url), (?).type))

  // repository_fragment
  //
  // Some repository types (normally version control-based) can be
  // fragmented. For example, a git repository consists of multiple commits
  // (fragments) which could contain different sets of packages and even
  // prerequisite/complement repositories. Note also that the same fragment
  // could be shared by multiple repository objects.
  //
  // For repository types that do not support fragmentation, there should
  // be a single repository_fragment with the name and location equal to the
  // ones of the containing repository. Such a fragment cannot be shared.
  //
  class repository;

  #pragma db object pointer(shared_ptr) session
  class repository_fragment
  {
  public:
    // Repository fragment id is a repository canonical name that identifies
    // just this fragment (for example, for git it is a canonical name of
    // the repository URL with the full, non-abbreviated commit id).
    //
    // Note that while this works naturally for git where the fragment (full
    // commit id) is also a valid fragment filter, it may not fit some future
    // repository types. Let's deal with it when we see such a beast.
    //
    string name; // Object id (canonical name).

    // For version control-based repositories it is used for a package
    // checkout, that may involve communication with the remote repository.
    //
    repository_location location;

    // We use a weak pointer for prerequisite repositories because we could
    // have cycles.
    //
    // Note that we could have cycles for complements via the root repository
    // that is the default complement for dir and git repositories (see
    // rep-fetch for details), and so we use a weak pointer for complements
    // either.
    //
    // Also note that these point to repositories, not repository fragments.
    //
    using dependencies = std::set<lazy_weak_ptr<repository>, compare_lazy_ptr>;

    dependencies complements;
    dependencies prerequisites;

  public:
    explicit
    repository_fragment (repository_location l)
        : location (move (l))
    {
      name = location.canonical_name ();
    }

    // Database mapping.
    //
    #pragma db member(name) id

    #pragma db member(location) column("")                       \
      set(this.location = std::move (?);                         \
          assert (this.name == this.location.canonical_name ()))

    #pragma db member(complements) id_column("repository_fragment") \
      value_column("complement") value_not_null

    #pragma db member(prerequisites) id_column("repository_fragment") \
      value_column("prerequisite") value_not_null

  private:
    friend class odb::access;
    repository_fragment () = default;
  };

  #pragma db view object(repository_fragment) \
    query(repository_fragment::name != "" && (?))
  struct repository_fragment_count
  {
    #pragma db column("count(*)")
    size_t result;

    operator size_t () const {return result;}
  };

  // repository
  //
  #pragma db object pointer(shared_ptr) session
  class repository
  {
  public:
    #pragma db value
    struct fragment_type
    {
      string friendly_name; // User-friendly fragment name (e.g, tag, etc).
      lazy_shared_ptr<repository_fragment> fragment;
    };

    using fragments_type = small_vector<fragment_type, 1>;

    string              name;        // Object id (canonical name).
    repository_location location;
    optional<string>    certificate; // PEM representation.
    fragments_type      fragments;

  public:
    explicit
    repository (repository_location l): location (move (l))
    {
      name = location.canonical_name ();
    }

    // Database mapping.
    //
    #pragma db member(name) id

    #pragma db member(location) column("")                       \
      set(this.location = std::move (?);                         \
          assert (this.name == this.location.canonical_name ()))

    #pragma db member(fragments) id_column("repository") \
      value_column("") value_not_null

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
    lazy_shared_ptr<bpkg::repository_fragment> repository_fragment;
    path location; // Package location within the repository fragment.
  };

  // dependencies
  //
  // Note on the terminology: we use the term "dependency" or "dependency
  // package" to refer to a general concept of package dependency. This would
  // include dependency alternatives, optional/conditional dependencies, etc.
  //
  // In contrast, below we use (mostly internally) the term "prerequisite
  // package" to refer to the "effective" dependency that has been resolved to
  // the actual package object.
  //
  #pragma db value(version_constraint) definition
  #pragma db value(dependency) definition
  #pragma db member(dependency::constraint) column("")
  #pragma db value(dependency_alternatives) definition

  // Extend dependency_alternatives to also represent the special test
  // dependencies of the test packages to the main packages, produced by
  // inverting the main packages external test dependencies (specified with
  // the tests, etc., manifest values).
  //
  #pragma db value
  class dependency_alternatives_ex: public dependency_alternatives
  {
  public:
    optional<test_dependency_type> type;

    dependency_alternatives_ex () = default;

    // Create the regular dependency alternatives object.
    //
    dependency_alternatives_ex (dependency_alternatives da)
        : dependency_alternatives (move (da)) {}

    // Create the special test dependencies object (built incrementally).
    //
    dependency_alternatives_ex (test_dependency_type t)
        : dependency_alternatives (false /* conditional */,
                                   false /* buildtime */,
                                   "" /* comment */),
          type (t) {}
  };

  using dependencies = vector<dependency_alternatives_ex>;

  // Convert the regular dependency alternatives list (normally comes from a
  // package manifest) to the extended version of it (see above).
  //
  inline dependencies
  convert (vector<dependency_alternatives>&& das)
  {
    return dependencies (make_move_iterator (das.begin ()),
                         make_move_iterator (das.end ()));
  }

  // tests
  //
  #pragma db value(test_dependency) definition

  using optional_test_dependency_type = optional<test_dependency_type>;

  #pragma db map type(test_dependency_type) as(string) \
    to(to_string (?))                                  \
    from(bpkg::to_test_dependency_type (?))

  #pragma db map type(optional_test_dependency_type)      \
   as(bpkg::optional_string)                              \
    to((?) ? to_string (*(?)) : bpkg::optional_string ()) \
    from((?)                                              \
         ? bpkg::to_test_dependency_type (*(?))           \
         : bpkg::optional_test_dependency_type ())

  // Wildcard version. Satisfies any version constraint and is represented as
  // 0+0 (which is also the "stub version"; since a real version is always
  // greater than the stub version, we reuse it to signify a special case).
  //
  extern const version wildcard_version;

  // package_name
  //
  #pragma db value(package_name) type("TEXT") options("COLLATE NOCASE")

  // available_package
  //
  #pragma db value
  struct available_package_id
  {
    package_name name;
    canonical_version version;

    available_package_id () = default;
    available_package_id (package_name, const bpkg::version&);
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

    // List of repository fragments to which this package version belongs
    // (yes, in our world, it can be in multiple, unrelated repositories)
    // together with locations within these repository fragments.
    //
    // Note that if the entry is the special root repository fragment (its
    // location is empty), then this is a transient (or "fake") object for an
    // existing package archive or package directory. In this case the
    // location is the path to the archive/directory and to determine which
    // one it is, use file/dir_exists(). While on the topic of fake
    // available_package objects, when one is created for a selected package
    // (see make_available()), this list is left empty with the thinking being
    // that since the package is already in at least fetched state, we
    // shouldn't be needing its location.
    //
    small_vector<package_location, 1> locations;

    // Package manifest data and, potentially, the special test dependencies.
    //
    // Note that there can be only one special test dependencies entry in the
    // list and it's always the last one, if present.
    //
    using dependencies_type = bpkg::dependencies;

    dependencies_type dependencies;

    small_vector<test_dependency, 1> tests;

    // Present for non-transient objects only (and only for certain repository
    // types).
    //
    optional<string> sha256sum;

  private:
    #pragma db transient
    mutable optional<version_type> system_version_;

  public:
    // Note: version constraints must be complete.
    //
    available_package (package_manifest&& m)
        : id (move (m.name), m.version),
          version (move (m.version)),
          dependencies (convert (move (m.dependencies))),
          tests (move (m.tests)),
          sha256sum (move (m.sha256sum)) {}

    // Create available stub package.
    //
    available_package (package_name n)
        : id (move (n), wildcard_version),
          version (wildcard_version) {}

    // Create a stub available package with a fixed system version. This
    // constructor is only used to create transient/fake available packages
    // based on the system selected packages.
    //
    available_package (package_name n, version_type sysv)
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
    using _dependency_key = odb::nested_key<dependency_alternatives_ex>;
    using _dependency_alternatives_ex_type =
      std::map<_dependency_key, dependency>;

    #pragma db value(_dependency_key)
    #pragma db member(_dependency_key::outer) column("dependency_index")
    #pragma db member(_dependency_key::inner) column("index")

    #pragma db member(dependencies) id_column("") value_column("")
    #pragma db member(dependency_alternatives_ex)             \
      table("available_package_dependency_alternatives")      \
      virtual(_dependency_alternatives_ex_type)               \
      after(dependencies)                                     \
      get(odb::nested_get (this.dependencies))                \
      set(odb::nested_set (this.dependencies, std::move (?))) \
      id_column("") key_column("") value_column("dep_")

    // tests
    //
    #pragma db member(tests) id_column("") value_column("test_")

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

  // Return the list of available test packages, that is, that are referred to
  // as external tests by some main package(s).
  //
  // Note that there can be only one test dependency row per package, so the
  // DISTINCT clause is not required.
  //
  #pragma db view object(available_package = package)                       \
    table("available_package_dependencies" = "pd" inner:                    \
          "pd.type IN ('tests', 'examples', 'benchmarks') AND "             \
          "pd.name = " + package::id.name + "AND" +                         \
          "pd.version_epoch = " + package::id.version.epoch + "AND" +       \
          "pd.version_canonical_upstream = " +                              \
            package::id.version.canonical_upstream + "AND" +                \
          "pd.version_canonical_release = " +                               \
            package::id.version.canonical_release + "AND" +                 \
          "pd.version_revision = " + package::id.version.revision + "AND" + \
          "pd.version_iteration = " + package::id.version.iteration)
  struct available_test
  {
    shared_ptr<available_package> package;
  };

  // Return the list of available main packages, that is, that refer to some
  // external test packages.
  //
  #pragma db view object(available_package = package)                       \
    table("available_package_tests" = "pt" inner:                           \
          "pt.name = " + package::id.name + "AND" +                         \
          "pt.version_epoch = " + package::id.version.epoch + "AND" +       \
          "pt.version_canonical_upstream = " +                              \
            package::id.version.canonical_upstream + "AND" +                \
          "pt.version_canonical_release = " +                               \
            package::id.version.canonical_release + "AND" +                 \
          "pt.version_revision = " + package::id.version.revision + "AND" + \
          "pt.version_iteration = " + package::id.version.iteration)        \
    query(distinct)
  struct available_main
  {
    shared_ptr<available_package> package;
  };

  // Query the available packages that optionally satisfy the specified
  // version constraint and return them in the version descending order, by
  // default. Note that a stub satisfies any constraint.
  //
  odb::result<available_package>
  query_available (database&,
                   const package_name&,
                   const optional<version_constraint>&,
                   bool order = true);

  // Only return packages that are in the specified repository fragments, their
  // complements or prerequisites (if prereq is true), recursively. While you
  // could maybe come up with a (barely comprehensible) view/query to achieve
  // this, doing it on the "client side" is definitely more straightforward.
  //
  vector<shared_ptr<available_package>>
  filter (const shared_ptr<repository_fragment>&,
          odb::result<available_package>&&,
          bool prereq = true);

  pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>
  filter_one (const shared_ptr<repository_fragment>&,
              odb::result<available_package>&&,
              bool prereq = true);

  shared_ptr<repository_fragment>
  filter (const shared_ptr<repository_fragment>&,
          const shared_ptr<available_package>&,
          bool prereq = true);

  vector<pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>>
  filter (const vector<shared_ptr<repository_fragment>>&,
          odb::result<available_package>&&,
          bool prereq = true);

  pair<shared_ptr<available_package>, shared_ptr<repository_fragment>>
  filter_one (const vector<shared_ptr<repository_fragment>>&,
              odb::result<available_package>&&,
              bool prereq = true);

  // Check if there are packages available in the configuration. If that's not
  // the case then print the info message into the diag record or, if it is
  // NULL, print the error message and fail.
  //
  void
  check_any_available (const dir_path& configuration,
                       transaction&,
                       const diag_record* = nullptr);

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
  // Return the package name in the [sys:]<name>[/<version>] form. The version
  // component is represented with the "/*" string for the wildcard version and
  // is omitted for the empty one.
  //
  string
  package_string (const package_name& name,
                  const version&,
                  bool system = false);

  // Return the package name in the [sys:]<name>[<version-constraint>] form.
  // The version constraint component is represented with the "/<version>"
  // string for the `== <version>` constraint, "/*" string for the wildcard
  // version, and is omitted for nullopt.
  //
  // If the version constraint other than the equality operator is specified
  // for a system package, return the "sys:<name>/..." string (with "..."
  // literally). This, in particular, is used for issuing diagnostics that
  // advises the user to configure a system package. Note that in this case
  // the user can only specify a specific version/wildcard on the command
  // line.
  //
  string
  package_string (const package_name& name,
                  const optional<version_constraint>&,
                  bool system = false);

  // A map of "effective" prerequisites (i.e., pointers to other selected
  // packages) to optional version constraint. Note that because it is a
  // single constraint, we don't support multiple dependencies on the same
  // package (e.g., two ranges of versions). See pkg_configure().
  //
  class selected_package;

  using package_prerequisites = std::map<lazy_shared_ptr<selected_package>,
                                         optional<version_constraint>,
                                         compare_lazy_ptr>;

  #pragma db object pointer(shared_ptr) session
  class selected_package
  {
  public:
    using version_type = bpkg::version;

    package_name name; // Object id.
    version_type version;
    package_state state;
    package_substate substate;

    // The hold flags indicate whether this package and/or version should be
    // retained in the configuration. A held package will not be automatically
    // removed. A held version will not be automatically upgraded. Note also
    // that the two flags are orthogonal: we may want to keep a specific
    // version of the package as long as it has dependents.
    //
    bool hold_package;
    bool hold_version;

    // Repository fragment from which this package came. Note that it is not a
    // pointer to the repository_fragment object because it could be wiped out
    // (e.g., as a result of rep-fetch). We call such packages "orphans".
    // While we can get a list of orphan's prerequisites (by loading its
    // manifest), we wouldn't know which repository fragment to use as a base
    // to resolve them. As a result, an orphan that is not already configured
    // (and thus has all its prerequisites resolved) is not very useful and
    // can only be purged.
    //
    repository_location repository_fragment;

    // Path to the archive of this package, if any. If not absolute, then it
    // is relative to the configuration directory. The purge flag indicates
    // whether the archive should be removed when the packaged is purged. If
    // the archive is not present, it should be false.
    //
    optional<path> archive;
    bool purge_archive;

    // Path to the source directory of this package, if any. If not absolute,
    // then it is relative to the configuration directory. The purge flag
    // indicates whether the directory should be removed when the packaged is
    // purged. If the source directory is not present, it should be false.
    //
    optional<dir_path> src_root;
    bool purge_src;

    // The checksum of the manifest file located in the source directory.
    //
    // Must be present if the source directory is present, unless the object
    // is created/updated during the package build simulation (see pkg-build
    // for details). Note that during the simulation the manifest may not be
    // available.
    //
    optional<std::string> manifest_checksum;

    // Path to the output directory of this package, if any. It is
    // always relative to the configuration directory, and is <name>
    // for external packages and <name>-<version> for others. It is
    // only set once the package is configured and its main purse is
    // to keep track of what needs to be cleaned by the user before
    // a broken package can be purged. Note that it could be the
    // same as src_root.
    //
    optional<dir_path> out_root;

    package_prerequisites prerequisites;

  public:
    bool
    system () const
    {
      // The system substate is only valid for the configured state.
      //
      assert (substate != package_substate::system ||
              state == package_state::configured);

      return substate == package_substate::system;
    }

    bool
    external () const
    {
      return
        // pkg-unpack <name>/<version>
        //
        (!repository_fragment.empty () &&
         repository_fragment.directory_based ()) ||

        // pkg-unpack --existing <dir>
        //
        (repository_fragment.empty () && !archive);
    }

    // Represent the wildcard version with the "*" string. Represent naturally
    // all other versions.
    //
    std::string
    version_string () const;

    std::string
    string () const {return package_string (name, version, system ());}

    // Return the relative source directory completed using the configuration
    // directory. Return the absolute source directory as is.
    //
    dir_path
    effective_src_root (const dir_path& configuration) const
    {
      // Cast for compiling with ODB (see above).
      //
      assert (static_cast<bool> (src_root));
      return src_root->absolute () ? *src_root : configuration / *src_root;
    }

    // Return the output directory using the configuration directory. Note
    // that the output directory is always relative.
    //
    dir_path
    effective_out_root (const dir_path& configuration) const
    {
      // Cast for compiling with ODB (see above).
      //
      assert (static_cast<bool> (out_root));
      return configuration / *out_root;
    }

    // Database mapping.
    //
    #pragma db member(name) id

    #pragma db member(prerequisites) id_column("package")      \
      key_column("prerequisite") key_not_null value_column("")

    // Explicit aggregate initialization for C++20 (private default ctor).
    //
    selected_package (package_name n,
                      version_type v,
                      package_state s,
                      package_substate ss,
                      bool hp,
                      bool hv,
                      repository_location rf,
                      optional<path> a,
                      bool pa,
                      optional<dir_path> sr,
                      bool ps,
                      optional<std::string> mc,
                      optional<dir_path> o,
                      package_prerequisites pps)
    : name (move (n)),
      version (move (v)),
      state (s),
      substate (ss),
      hold_package (hp),
      hold_version (hv),
      repository_fragment (move (rf)),
      archive (move (a)),
      purge_archive (pa),
      src_root (move (sr)),
      purge_src (ps),
      manifest_checksum (move (mc)),
      out_root (move (o)),
      prerequisites (move (pps)) {}

  private:
    friend class odb::access;
    selected_package () = default;
  };

  inline ostream&
  operator<< (ostream& os, const selected_package& p)
  {
    return os << p.string ();
  }

  // Check if the directory containing the specified package version should be
  // considered its iteration. Return the version of this iteration if that's
  // the case and nullopt otherwise.
  //
  // Notes:
  //
  // - The package directory is considered an iteration of the package if this
  //   upstream version and revision is already present (selected) in the
  //   configuration and has a source directory. If that's the case, then the
  //   specified directory path and the checksum of the manifest file it
  //   contains are compared to the ones of the package present in the
  //   configuration. If both match, then the present package version
  //   (including its iteration, if any) is returned. Otherwise (the package
  //   has moved and/or the packaging information has changed), the present
  //   package version with the incremented iteration number is returned. Note
  //   that the directory path is matched only for the external selected
  //   packages.
  //
  // - Only a single package iteration is valid per version in the
  //   configuration. This, in particular, means that a package of the
  //   specific upstream version and revision shouldn't come from multiple
  //   external (source) directories.
  //
  //   If requested, the function checks if an external package of this
  //   upstream version and revision is already available in the configuration
  //   and fails if that's the case.
  //
  // - The manifest file located in the specified directory is not parsed, and
  //   so is not checked to match the specified package name and version.
  //
  class common_options;

  optional<version>
  package_iteration (const common_options&,
                     const dir_path& configuration,
                     transaction&,
                     const dir_path&,
                     const package_name&,
                     const version&,
                     bool check_external);

  // certificate
  //
  // Information extracted from a repository X.509 certificate. The actual
  // certificate is stored on disk as .bpkg/certs/<id>.pem (we have to store
  // it as a file because that's the only way to pass it to openssl).
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
  // certificate would). The id member for such a dummy certificate contains
  // the truncated to 16 chars SHA256 checksum of this name. Members other then
  // name and id are meaningless for the dummy certificate.
  //
  #pragma db object pointer(shared_ptr) session
  class certificate
  {
  public:
    string id;          // SHA256 fingerprint truncated to 16 characters.
    string fingerprint; // Fingerprint canonical representation.

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
      return butl::system_clock::now () > end_date;
    }

  public:
    certificate (string i,
                 string f,
                 string n,
                 string o,
                 string e,
                 timestamp sd,
                 timestamp ed)
        : id (move (i)),
          fingerprint (move (f)),
          name (move (n)),
          organization (move (o)),
          email (move (e)),
          start_date (move (sd)),
          end_date (move (ed))
    {
    }

    // Create dummy certificate.
    //
    certificate (string i, string n)
        : id (move (i)),
          name (move (n)),
          start_date (timestamp_unknown),
          end_date (timestamp_unknown)
    {
    }

    // Database mapping.
    //
    #pragma db member(id) id

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
    optional<version_constraint> constraint;
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
    package_name name;

    #pragma db column("pp.")
    optional<version_constraint> constraint;
  };

  // Return a count of repositories that contain this repository fragment.
  //
  #pragma db view table("repository_fragments")
  struct fragment_repository_count
  {
    #pragma db column("count(*)")
    size_t result;

    operator size_t () const {return result;}
  };

  // Return a list of repositories that contain this repository fragment.
  //
  #pragma db view object(repository)                      \
    table("repository_fragments" = "rfs" inner:           \
          "rfs.repository = " + repository::name)         \
    object(repository_fragment inner: "rfs.fragment = " + \
           repository_fragment::name)
  struct fragment_repository
  {
    shared_ptr<repository> object;

    operator const shared_ptr<repository> () const {return object;}
  };

  // Return a list of repository fragments that depend on this repository as a
  // complement.
  //
  #pragma db view object(repository = complement)                    \
    table("repository_fragment_complements" = "rfc" inner:           \
          "rfc.complement = " + complement::name)                    \
    object(repository_fragment inner: "rfc.repository_fragment = " + \
           repository_fragment::name)
  struct repository_complement_dependent
  {
    shared_ptr<repository_fragment> object;

    operator const shared_ptr<repository_fragment> () const {return object;}
  };

  // Return a list of repository fragments that depend on this repository as a
  // prerequisite.
  //
  #pragma db view object(repository = prerequisite)                  \
    table("repository_fragment_prerequisites" = "rfp" inner:         \
          "rfp.prerequisite = " + prerequisite::name)                \
    object(repository_fragment inner: "rfp.repository_fragment = " + \
           repository_fragment::name)
  struct repository_prerequisite_dependent
  {
    shared_ptr<repository_fragment> object;

    operator const shared_ptr<repository_fragment> () const {return object;}
  };

  // Return a list of packages available from this repository fragment.
  //
  #pragma db view object(repository_fragment)                                \
    table("available_package_locations" = "pl" inner:                        \
          "pl.repository_fragment = " + repository_fragment::name)           \
    object(available_package = package inner:                                \
           "pl.name = " + package::id.name + "AND" +                         \
           "pl.version_epoch = " + package::id.version.epoch + "AND" +       \
           "pl.version_canonical_upstream = " +                              \
             package::id.version.canonical_upstream + "AND" +                \
           "pl.version_canonical_release = " +                               \
             package::id.version.canonical_release + "AND" +                 \
           "pl.version_revision = " + package::id.version.revision + "AND" + \
           "pl.version_iteration = " + package::id.version.iteration)
  struct repository_fragment_package
  {
    shared_ptr<available_package> package; // Must match the alias (see above).

    operator const shared_ptr<available_package> () const {return package;}
  };

  // Return a list of repository fragments the packages come from.
  //
  #pragma db view object(repository_fragment)                                \
    table("available_package_locations" = "pl" inner:                        \
          "pl.repository_fragment = " + repository_fragment::name)           \
    object(available_package = package inner:                                \
           "pl.name = " + package::id.name + "AND" +                         \
           "pl.version_epoch = " + package::id.version.epoch + "AND" +       \
           "pl.version_canonical_upstream = " +                              \
             package::id.version.canonical_upstream + "AND" +                \
           "pl.version_canonical_release = " +                               \
             package::id.version.canonical_release + "AND" +                 \
           "pl.version_revision = " + package::id.version.revision + "AND" + \
           "pl.version_iteration = " + package::id.version.iteration)
  struct package_repository_fragment
  {
    #pragma db column(package::id)
    available_package_id package_id;

    shared_ptr<bpkg::repository_fragment> repository_fragment;
  };

  // Version comparison operators.
  //
  // They allow comparing objects that have epoch, canonical_upstream,
  // canonical_release, revision, and iteration data members. The idea is that
  // this works for both query members of types version and canonical_version.
  // Note, though, that the object revisions should be comparable (both
  // optional, numeric, etc), so to compare version to query member or
  // canonical_version you may need to explicitly convert the version object
  // to canonical_version.
  //
  // Also note that if the comparison operation ignores the revision, then it
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
    auto r (x.epoch == y.epoch &&
            x.canonical_upstream == y.canonical_upstream &&
            x.canonical_release == y.canonical_release);

    return !revision
      ? r
      : !iteration
        ? r && x.revision == y.revision
        : r && x.revision == y.revision && x.iteration == y.iteration;
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

    auto r (x.epoch != y.epoch ||
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

    auto r (
      x.epoch < y.epoch ||
      (x.epoch == y.epoch && x.canonical_upstream < y.canonical_upstream) ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release < y.canonical_release));

    if (revision)
    {
      r = r || (x.epoch == y.epoch &&
                x.canonical_upstream == y.canonical_upstream &&
                x.canonical_release == y.canonical_release &&
                x.revision < y.revision);

      if (iteration)
        r = r || (x.epoch == y.epoch &&
                  x.canonical_upstream == y.canonical_upstream &&
                  x.canonical_release == y.canonical_release &&
                  x.revision == y.revision &&
                  x.iteration < y.iteration);
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
      r = r || (x.epoch == y.epoch &&
                x.canonical_upstream == y.canonical_upstream &&
                x.canonical_release <= y.canonical_release);
    }
    else
    {
      r = r || (x.epoch == y.epoch &&
                x.canonical_upstream == y.canonical_upstream &&
                x.canonical_release < y.canonical_release);

      if (!iteration)
        r = r || (x.epoch == y.epoch &&
                  x.canonical_upstream == y.canonical_upstream &&
                  x.canonical_release == y.canonical_release &&
                  x.revision <= y.revision);
      else
        r =  r ||

          (x.epoch == y.epoch &&
           x.canonical_upstream == y.canonical_upstream &&
           x.canonical_release == y.canonical_release &&
           x.revision < y.revision) ||

          (x.epoch == y.epoch &&
           x.canonical_upstream == y.canonical_upstream &&
           x.canonical_release == y.canonical_release &&
           x.revision == y.revision &&
           x.iteration <= y.iteration);
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
      (x.epoch == y.epoch && x.canonical_upstream > y.canonical_upstream) ||
      (x.epoch == y.epoch && x.canonical_upstream == y.canonical_upstream &&
       x.canonical_release > y.canonical_release));

    if (revision)
    {
      r = r || (x.epoch == y.epoch &&
                x.canonical_upstream == y.canonical_upstream &&
                x.canonical_release == y.canonical_release &&
                x.revision > y.revision);

      if (iteration)
        r = r || (x.epoch == y.epoch &&
                  x.canonical_upstream == y.canonical_upstream &&
                  x.canonical_release == y.canonical_release &&
                  x.revision == y.revision &&
                  x.iteration > y.iteration);
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
      r = r || (x.epoch == y.epoch &&
                x.canonical_upstream == y.canonical_upstream &&
                x.canonical_release >= y.canonical_release);
    }
    else
    {
      r = r || (x.epoch == y.epoch &&
                x.canonical_upstream == y.canonical_upstream &&
                x.canonical_release > y.canonical_release);

      if (!iteration)
        r = r || (x.epoch == y.epoch &&
                  x.canonical_upstream == y.canonical_upstream &&
                  x.canonical_release == y.canonical_release &&
                  x.revision >= y.revision);
      else
        r =  r ||

          (x.epoch == y.epoch &&
           x.canonical_upstream == y.canonical_upstream &&
           x.canonical_release == y.canonical_release &&
           x.revision > y.revision) ||

          (x.epoch == y.epoch &&
           x.canonical_upstream == y.canonical_upstream &&
           x.canonical_release == y.canonical_release &&
           x.revision == y.revision &&
           x.iteration >= y.iteration);
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
}

// Workaround for GCC __is_invocable/non-constant condition bug (#86441).
//
#ifdef ODB_COMPILER
namespace std
{
  template class map<bpkg::available_package::_dependency_key,
                     bpkg::dependency>;
}
#endif

#include <bpkg/package.ixx>

#endif // BPKG_PACKAGE_HXX
