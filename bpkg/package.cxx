// file      : bpkg/package.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>

#include <sstream>

#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <bpkg/database.hxx>
#include <bpkg/checksum.hxx>
#include <bpkg/rep-mask.hxx>
#include <bpkg/pkg-verify.hxx>
#include <bpkg/diagnostics.hxx>
#include <bpkg/satisfaction.hxx>
#include <bpkg/manifest-utility.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  const version wildcard_version (0, "0", nullopt, nullopt, 0);

  // configuration
  //
  configuration::
  configuration (optional<string> n,
                 string t,
                 optional<string> fcm,
                 optional<uuid_type> uid)
      : id (0),
        name (move (n)),
        type (move (t)),
        expl (false),
        fetch_cache_mode (move (fcm))
  {
    try
    {
      uuid = uid ? *uid : uuid_type::generate ();
    }
    catch (const system_error& e)
    {
      fail << "unable to generate configuration uuid: " << e;
    }
  }

  dir_path configuration::
  effective_path (const dir_path& d) const
  {
    if (path.relative ())
    {
      dir_path r (d / path);

      string what ("linked with " + d.representation () + " configuration " +
                   (name ? *name : to_string (*id)));

      normalize (r, what.c_str ());
      return r;
    }
    else
      return path;
  }

  // package_key
  //
  string package_key::
  string () const
  {
    const std::string& s (db.get ().string);
    return !s.empty () ? name.string () + ' ' + s : name.string ();
  }

  bool package_key::
  operator< (const package_key& v) const
  {
    int r (name.compare (v.name));
    return r != 0 ? (r < 0) : (db < v.db);
  }

  // package_version_key
  //
  string package_version_key::
  string (bool ignore_version) const
  {
    std::string r (name.string ());

    if (version && !version->empty () && !ignore_version)
    {
      r += '/';
      r += version->string ();
    }

    const std::string& d (db.get ().string);

    if (!d.empty ())
    {
      r += ' ';
      r += d;
    }

    return r;
  }

  bool package_version_key::
  operator< (const package_version_key& v) const
  {
    // NOTE: remember to update cmdline_adjustments::tried_earlier() if
    // changing anything here.
    //
    if (int r = name.compare (v.name))
      return r < 0;

    return version != v.version ? (version < v.version) : (db < v.db);
  }

  // available_package
  //

  // Return the available package manifest.
  //
  // The available package manifest starts with the header manifest and is
  // followed by the package manifest. For example:
  /*
     : 1
     version: 1
     test-dependency-type: tests
     test-dependency-index: 4
     :
     name: foo-tests
     version: 1.0.0
     type: tests
     language: c++
     project: foo
     depends: * build2 >= 0.18.0
     depends: * bpkg >= 0.18.0
     depends: libfoo-bar == 1.0.0 ? ($bar)
     depends: libfoo-baz == 1.0.0 ? ($baz)
     depends: libfoo-bar == 1.0.0 ? (!$defined(config.foo_tests.api)) $config.foo_tests.api=bar | \
              libfoo-baz == 1.0.0 ? (!$defined(config.foo_tests.api)) $config.foo_tests.api=baz
     bootstrap-build:
     \
     project = foo-tests
     ...
     \
     root-build:
     \
     config [string] config.foo_tests.api
     bar = ($config.foo_tests.api == 'bar')
     baz = ($config.foo_tests.api == 'baz')
     ...
     \
  */
  // The header manifest values:
  //
  // version: <number>
  // [test-dependency-type]: <type>
  // [test-dependency-index]: <number>
  //
  // <type> = tests | examples | benchmarks
  //
  // The mandatory version value specify the version of the available package
  // manifest. In the absence of proper database migration, the manifest
  // parsing can use this value to recognize older manifests and adapt to
  // them.
  //
  // The test-dependency-* values, if present, specify which of the subsequent
  // package manifest depends clauses is the special inverse test dependency
  // and what is its type.
  //
  // The package manifest values:
  //
  // name version upstream-version
  // type language
  // project
  // depends
  // tests examples benchmarks
  // bootstrap-build root-build *-build
  // *-name *-version *-to-downstream-version
  // sha256sum
  //
  // See the build2 Package Manager manual for the package manifest value
  // definitions.
  //
  string available_package::
  manifest () const
  {
    // NOTE: consider incrementing the database schema version and/or manifest
    //       version and migrating if changing anything here.

    assert (!stub ()                     &&
            bootstrap_build.has_value () &&
            alt_naming.has_value ());

    try
    {
      ostringstream os;
      manifest_serializer s (os, "<string>");

      // Serialize the available package header manifest.
      //
      s.next ("", "1"); // Start of available package header manifest.

      // Serialize the current available package manifest version.
      //
      s.next ("version", "1");

      // Serialize the type and index of the special inverse test dependency,
      // if present. Note: there can only be at most one.
      //
      for (size_t i (0); i != dependencies.size (); ++i)
      {
        const dependency_alternatives_ex& das (dependencies[i]);

        if (das.type)
        {
          s.next ("test-dependency-type", to_string (*das.type));
          s.next ("test-dependency-index", to_string (i));
          break;
        }
      }

      s.next ("", "");  // End of available package header manifest.

      // Serialize the package manifest.
      //
      // The serialization code is based on the libbpkg's
      // serialize_package_manifest() function implementation.
      //
      s.next ("", "1"); // Start of package manifest.

      s.next ("name", id.name.string ());
      s.next ("version", version.string ());

      if (upstream_version)
        s.next ("upstream-version", *upstream_version);

      if (type)
        s.next ("type", *type);

      for (const language& l: languages)
        s.next ("language", !l.impl ? l.name : l.name + "=impl");

      if (project)
        s.next ("project", project->string ());

      for (const dependency_alternatives& das: dependencies)
        s.next ("depends", das.string ());

      for (const test_dependency& t: tests)
        s.next (to_string (t.type), t.string ());

      s.next (*alt_naming ? "bootstrap-build2" : "bootstrap-build",
              *bootstrap_build);

      if (root_build)
        s.next (*alt_naming ? "root-build2" : "root-build", *root_build);

      for (const auto& bf: buildfiles)
        s.next (bf.path.posix_string () + (*alt_naming ? "-build2" : "-build"),
                bf.content);

      for (const distribution_name_value& nv: distribution_values)
        s.next (nv.name, nv.value);

      if (sha256sum)
        s.next ("sha256sum", *sha256sum);

      s.next ("", ""); // End of package manifest.
      s.next ("", ""); // End of stream.

      return os.str ();
    }
    catch (const manifest_serialization& e)
    {
      // We shouldn't be creating a non-serializable manifest, since it's
      // crafted from the parsed values. Unless there are some backward
      // compatibility issues (available packages were not properly migrated,
      // etc).
      //
      fail << "unable to serialize available package manifest for "
           << package_string (id.name, version) << ": " << e.description
           << endf;
    }
    catch (const io_error& e)
    {
      // This shouldn't normally happen, since we are serializing into the
      // string stream. Let's still handle this exception for good measure.
      //
      fail << "unable to write available package manifest for "
           << package_string (id.name, version) << ": " << e << endf;
    }
  }

  available_package::
  available_package (const string& s)
  {
    try
    {
      // Parse the available package manifest (see
      // available_package::manifest() for the manifest description).
      //
      istringstream is (s);
      manifest_parser p (is, "<string>");
      manifest_name_value nv (p.next ());

      auto bad_name ([&p, &nv](const string& d) {
        throw manifest_parsing (p.name (), nv.name_line, nv.name_column, d);});

      auto bad_value ([&p, &nv](const string& d) {
        throw manifest_parsing (p.name (), nv.value_line, nv.value_column, d);});

      // Parse the available package header manifest.
      //
      // Make sure this is the start and we support the version.
      //
      if (!nv.name.empty ())
        bad_name ("start of available package manifest expected");

      if (nv.value != "1")
        bad_value ("unsupported format version");

      // Parse the available package manifest version.
      //
      nv = p.next ();

      if (nv.name != "version")
        bad_name ("available package manifest version expected");

      // Note that we will ignore unknown values in both header and package
      // manifests, assuming that this is harmless.
      //
      // Specifically, if we are parsing manifest created by a newer
      // toolchain, then if just skipping an unknown value wouldn't be enough,
      // that newer toolchain would bump the database schema version and
      // performed manifest migration. In this case we would fail with the
      // 'configuration is too new' error while trying to open the
      // configuration database.
      //
      // If we are parsing manifest created by an older toolchain, an unknown
      // value can only be encountered if the manifests were not migrated by
      // this or some previous bpkg version intentionally, in the assumption
      // that skipping unknown values is harmless.
      //
      optional<test_dependency_type> tdt;
      optional<size_t> tdi;

      for (nv = p.next (); !nv.empty (); nv = p.next ())
      {
        string& n (nv.name);
        string& v (nv.value);

        if (n == "test-dependency-type")
        {
          try
          {
            tdt = to_test_dependency_type (v);
          }
          catch (const invalid_argument& e)
          {
            bad_value (e.what ());
          }
        }
        else if (n == "test-dependency-index")
        {
          if (optional<size_t> i =
              parse_number (v, numeric_limits<size_t>::max ()))
          {
            tdi = i;
          }
          else
            bad_value ("invalid inverse test dependency index");
        }
      }

      if (tdt.has_value () != tdi.has_value ())
        bad_value ("inverse test dependency type and index must both be "
                   "either specified or not");

      // Parse the package manifest.
      //
      nv = p.next ();

      if (!nv.name.empty ())
        bad_name ("start of package manifest expected");

      if (nv.value != "1")
        bad_value ("unsupported format version");

      // Note that the values are expected to already be completed/expanded.
      //
      package_manifest m (
        p,
        move (nv),
        true /* ignore_unknown */,
        false /* complete_values */,
        package_manifest_flags::forbid_file              |
        package_manifest_flags::forbid_fragment          |
        package_manifest_flags::forbid_incomplete_values |
        package_manifest_flags::require_bootstrap_build);

      if (tdi && *tdi >= m.dependencies.size ())
        bad_value ("inverse test dependency index " + to_string (*tdi) +
                   " is greater than number of dependencies " +
                   to_string (m.dependencies.size ()));

      *this = available_package (move (m));

      if (tdt)
        dependencies[*tdi].type = *tdt;
    }
    catch (const manifest_parsing& e)
    {
      // Normally, we shouldn't be failing on parsing this manifest, since
      // it's generated automatically by us. Unless there are some backward
      // compatibility issues (available package manifests were not properly
      // migrated, etc).
      //
      diag_record dr (fail (e.name, e.line, e.column));
      dr << "unable to parse available package manifest: " << e.description <<
        info << "manifest:\n" << s;
    }
    catch (const io_error& e)
    {
      // This shouldn't normally happen, since we are parsing from the string
      // stream. Let's still handle this exception for good measure.
      //
      fail << "unable to read available package manifest: " << e <<
        info << "manifest:\n" << s;
    }
  }

  const version* available_package::
  system_version (database& db) const
  {
    if (!system_version_)
    {
      assert (db.system_repository);

      if (const system_package* sp = db.system_repository->find (id.name))
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

  pair<const version*, bool> available_package::
  system_version_authoritative (database& db) const
  {
    assert (db.system_repository);

    const system_package* sp (db.system_repository->find (id.name));

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

  void
  check_any_available (const linked_databases& dbs,
                       transaction&,
                       const diag_record* drp)
  {
    bool rep (false);
    bool pkg (false);
    for (database& db: dbs)
    {
      if (db.query_value<repository_count> () != 0)
      {
        rep = true;

        if (db.query_value<available_package_count> () != 0)
        {
          pkg = true;
          break;
        }
      }
    }

    if (pkg)
      return;

    diag_record d;
    const diag_record& dr (drp != nullptr ? *drp << info : d << fail);

    if (dbs.size () == 1)
      dr << "configuration " << dbs[0].get ().config_orig << " has ";
    else
      dr << "specified configurations have ";

    if (!rep)
    {
      dr << "no repositories" <<
        info << "use 'bpkg rep-add' to add a repository";
    }
    else
    {
      dr << "no available packages" <<
        info << "use 'bpkg rep-fetch' to fetch available packages list";
    }
  }

  void
  check_any_available (database& db, transaction& t, const diag_record* dr)
  {
    return check_any_available (linked_databases ({db}), t, dr);
  }

  string
  package_string (const package_name& n, const version& v, bool system)
  {
    assert (!n.empty ());

    string vs (v.empty ()
               ? string ()
               : v == wildcard_version
                 ? "/*"
                 : '/' + v.string ());

    return system ? "sys:" + n.string () + vs : n.string () + vs;
  }

  string
  package_string (const package_name& name,
                  const optional<version_constraint>& constraint,
                  bool system)
  {
    // Fallback to the version type-based overload if the constraint is not
    // specified.
    //
    if (!constraint)
      return package_string (name, version (), system);

    // There are no scenarios where the version constrain is present but is
    // empty (both endpoints are nullopt).
    //
    assert (!constraint->empty ());

    // If the endpoint versions are equal then represent the constraint as the
    // "<name>/<version>" string rather than "<name> == <version>", using the
    // version type-based overload.
    //
    const optional<version>& min_ver (constraint->min_version);
    bool eq (min_ver == constraint->max_version);

    if (eq)
      return package_string (name, *min_ver, system);

    if (system)
      return package_string (name, version (), system) + "/...";

    // Quote the result as it contains the space character.
    //
    return '\'' + name.string () + ' ' + constraint->string () + '\'';
  }

  // selected_package
  //
  string selected_package::
  string (database& db) const
  {
    const std::string& s (db.string);
    return !s.empty () ? string () + ' ' + s : string ();
  }

  _selected_package_ref::
  _selected_package_ref (const lazy_shared_ptr<selected_package>& p)
      : configuration (p.database ().uuid),
        prerequisite (p.object_id ())
  {
  }

  lazy_shared_ptr<selected_package> _selected_package_ref::
  to_ptr (odb::database& db) &&
  {
    database& pdb (static_cast<database&> (db));

    // Note that if this points to a different configuration, then it should
    // already be pre-attached since it must be explicitly linked.
    //
    database& ddb (pdb.find_dependency_config (configuration));

    // Make sure the prerequisite exists in the explicitly linked
    // configuration, so that a subsequent load() call will not fail. This,
    // for example, can happen in unlikely but possible situation when the
    // implicitly linked configuration containing a dependent was temporarily
    // renamed before its prerequisite was dropped.
    //
    // Note that the diagnostics lacks information about the dependent and its
    // configuration. However, handling this situation at all the load()
    // function call sites where this information is available, for example by
    // catching the odb::object_not_persistent exception, feels a bit
    // hairy. Given the situation is not common, let's keep it simple for now
    // and see how it goes.
    //
    if (ddb != pdb && ddb.find<selected_package> (prerequisite) == nullptr)
      fail << "unable to find prerequisite package " << prerequisite
           << " in linked configuration " << ddb.config_orig;

    return lazy_shared_ptr<selected_package> (ddb, move (prerequisite));
  }

  string
  to_string (config_source s)
  {
    switch (s)
    {
    case config_source::user:      return "user";
    case config_source::dependent: return "dependent";
    case config_source::reflect:   return "reflect";
    }

    return string (); // Should never reach.
  }

  config_source
  to_config_source (const string& s)
  {
         if (s == "user")      return config_source::user;
    else if (s == "dependent") return config_source::dependent;
    else if (s == "reflect")   return config_source::reflect;
    else throw invalid_argument ("invalid config source '" + s + '\'');
  }

  shared_ptr<available_package>
  make_available (const common_options& options,
                  database& db,
                  const shared_ptr<selected_package>& sp)
  {
    assert (sp != nullptr && sp->state != package_state::broken);

    if (sp->system ())
      return make_shared<available_package> (sp->name, sp->version);

    // @@ PERF We should probably implement the available package caching not
    //    to parse the same manifests multiple times during all that build
    //    plan refinement iterations. What should be the cache key? Feels like
    //    it should be the archive/directory path. Note that the package
    //    manifests can potentially differ in different external package
    //    directories for the same version iteration. Testing showed 6%
    //    speedup on tests (debug/sanitized).
    //
    if (!sp->manifest_section.loaded ())
      db.load (*sp, sp->manifest_section);

    if (sp->manifest)
      return make_shared<available_package> (*sp->manifest);

    // @@ TMP For configurations created with the schema version 27 and above
    //        the manifest should always be present for the selected source
    //        packages. For earlier (but migrated) configurations it can be
    //        absent, in which case we will use the manifest file as a
    //        fallback. Given that this can result in a broken configuration
    //        (see pkg-build/dependent/external-tests/no-available-package
    //        test for details), we may want to remove this fallback early
    //        enough, say after 0.18.0 toolchain is released, and just fail
    //        instead.
    //
    //fail << "no repository information for " << *sp << db <<
    //  info << "upgrade or deorphan " << sp->name << db <<
    //  info << "run 'bpkg help pkg-build' for more information";
    //
    // The package is in at least fetched state, which means we should be able
    // to get its manifest.
    //
    package_manifest m (
      sp->state == package_state::fetched
      ? pkg_verify (options,
                    sp->effective_archive (db.config_orig),
                    true /* ignore_unknown */,
                    false /* ignore_toolchain */,
                    false /* expand_values */,
                    true /* load_buildfiles */)
      : pkg_verify (options,
                    sp->effective_src_root (db.config_orig),
                    true /* ignore_unknown */,
                    false /* ignore_toolchain */,
                    true /* load_buildfiles */,
                    // Copy potentially fixed up version from selected package.
                    [&sp] (version& v) {v = sp->version;}));

    return make_shared<available_package> (move (m));
  }

  pair<shared_ptr<selected_package>, database*>
  find_dependency (database& db, const package_name& pn, bool buildtime)
  {
    pair<shared_ptr<selected_package>, database*> r;

    for (database& ldb: db.dependency_configs (pn, buildtime))
    {
      shared_ptr<selected_package> p (ldb.find<selected_package> (pn));

      if (p != nullptr)
      {
        if (r.first == nullptr)
        {
          r.first = move (p);
          r.second = &ldb;
        }
        else
        {
          fail << "package " << pn << " appears in multiple configurations" <<
            info << r.first->state << " in " << r.second->config_orig <<
            info << p->state << " in " << ldb.config_orig;
        }
      }
    }

    return r;
  }

  optional<version>
  package_iteration (const common_options& o,
                     database& db,
                     transaction&,
                     const dir_path& d,
                     const package_name& n,
                     const version& v,
                     const package_info* pi,
                     bool check_external)
  {
    tracer trace ("package_iteration");

    tracer_guard tg (db, trace);

    if (check_external)
    {
      using query = query<package_repository_fragment>;

      query q (
        query::package::id.name == n &&
        compare_version_eq (query::package::id.version,
                            canonical_version (v),
                            true /* revision */,
                            false /* iteration */));

      for (const auto& prf: db.query<package_repository_fragment> (q))
      {
        const shared_ptr<repository_fragment>& rf (prf.repository_fragment);

        if (!rep_masked_fragment (db, rf) && rf->location.directory_based ())
          fail << "external package " << n << '/' << v
               << " is already available from "
               << rf->location.canonical_name ();
      }
    }

    shared_ptr<selected_package> p (db.find<selected_package> (n));

    if (p == nullptr || !p->src_root ||
        compare_version_ne (v,
                            p->version,
                            true /* revision */,
                            false /* iteration */))
      return nullopt;

    bool changed (!p->external ());

    // If the selected package is not external, then increment the iteration
    // number to make the external package preferable. Note that for such
    // packages the manifest/subprojects and buildfiles checksums are absent.
    //
    if (!changed)
    {
      // The selected package must not be "simulated" (see pkg-build for
      // details).
      //
      assert (p->manifest_checksum);

      changed = (package_checksum (o, d, pi) != *p->manifest_checksum);

      // If the manifest hasn't changed and the package has buildfile clauses
      // in the dependencies, then check if the buildfiles haven't changed
      // either.
      //
      if (!changed && p->buildfiles_checksum)
      {
        // Always calculate the checksum over the buildfiles since the package
        // is external.
        //
        changed = package_buildfiles_checksum (
          nullopt /* bootstrap_build */,
          nullopt /* root_build */,
          {}      /* buildfiles */,
          d) != *p->buildfiles_checksum;
      }

      // If the manifest hasn't changed but the selected package points to an
      // external source directory, then we also check if the directory have
      // moved.
      //
      if (!changed)
      {
        dir_path src_root (p->effective_src_root (db.config));

        // We need to complete and normalize the source directory as it may
        // generally be completed against the configuration directory
        // (unlikely but possible), that can be relative and/or not
        // normalized.
        //
        normalize (src_root, "package source");

        changed = src_root != normalize (d, "package source");
      }
    }

    return !changed
      ? p->version
      : version (v.epoch,
                 v.upstream,
                 v.release,
                 v.revision,
                 p->version.iteration + 1);
  }

  // state
  //
  string
  to_string (package_state s)
  {
    switch (s)
    {
    case package_state::transient:  return "transient";
    case package_state::broken:     return "broken";
    case package_state::fetched:    return "fetched";
    case package_state::unpacked:   return "unpacked";
    case package_state::configured: return "configured";
    }

    return string (); // Should never reach.
  }

  package_state
  to_package_state (const string& s)
  {
         if (s == "transient")  return package_state::transient;
    else if (s == "broken")     return package_state::broken;
    else if (s == "fetched")    return package_state::fetched;
    else if (s == "unpacked")   return package_state::unpacked;
    else if (s == "configured") return package_state::configured;
    else throw invalid_argument ("invalid package state '" + s + '\'');
  }

  // substate
  //
  string
  to_string (package_substate s)
  {
    switch (s)
    {
    case package_substate::none:   return "none";
    case package_substate::system: return "system";
    }

    return string (); // Should never reach.
  }

  package_substate
  to_package_substate (const string& s)
  {
         if (s == "none")   return package_substate::none;
    else if (s == "system") return package_substate::system;
    else throw invalid_argument ("invalid package substate '" + s + '\'');
  }

  // certificate
  //
  ostream&
  operator<< (ostream& os, const certificate& c)
  {
    using butl::operator<<;

    if (c.dummy ())
      os << c.name << " (dummy)";
    else
      os << c.name << ", \"" << c.organization << "\" <" << c.email << ">, "
         << c.start_date << " - " << c.end_date << ", " << c.fingerprint;

    return os;
  }

  // package_dependent
  //
  odb::result<package_dependent>
  query_dependents (database& db,
                    const package_name& dep,
                    database& dep_db)
  {
    // Prepare and cache this query since it's executed a lot. Note that we
    // have to cache one per database.
    //
    using query = query<package_dependent>;
    using prep_query = prepared_query<package_dependent>;

    struct params
    {
      string name;
      string config;     // Configuration UUID.
      string query_name;
    };

    // Note that we used to use the db.uuid.string () call to generate the
    // database identity component of the query name. This, however, turned
    // out to be quite slow, taking about the same time to execute as the
    // database query. Thus, we switched to using the database address as the
    // database identity, similar to the database comparison operators (see
    // database.hxx for details). With this approach the query name is
    // generated 5 times faster. Also note that both name generating
    // approaches assume that the databases are not detached during the cached
    // query lifetime (bpkg run).
    //
    string qn (to_string (reinterpret_cast<uintptr_t> (&db)));
    qn += "-package-dependent-query";

    params*    qp;
    prep_query pq (db.lookup_query<package_dependent> (qn.c_str (), qp));

    if (!pq)
    {
      unique_ptr<params> p (qp = new params ());
      p->query_name = move (qn);

      query q ("prerequisite = " + query::_ref (p->name) + "AND" +
               "configuration = " + query::_ref (p->config));

      pq = db.prepare_query<package_dependent> (p->query_name.c_str (), q);
      db.cache_query (pq, move (p));
    }

    qp->name   = dep.string ();
    qp->config = dep_db.uuid.string ();

    return pq.execute ();
  }

  vector<package_dependent>
  query_dependents_cache (database& db,
                          const package_name& dep,
                          database& dep_db)
  {
    vector<package_dependent> r;
    for (package_dependent& pd: query_dependents (db, dep, dep_db))
      r.push_back (move (pd));
    return r;
  }

  bool
  toolchain_buildtime_dependency (const common_options& o,
                                  const dependency_alternatives& das,
                                  const package_name* pkg)
  {
    if (das.buildtime)
    {
      for (const dependency_alternative& da: das)
      {
        for (const dependency& d: da)
        {
          const package_name& dn (d.name);

          if (dn == "build2")
          {
            if (pkg != nullptr && d.constraint && !satisfy_build2 (o, d))
            {
              fail << "unable to satisfy constraint (" << d << ") for "
                   << "package " << *pkg <<
                info << "available build2 version is " << build2_version;
            }

            return true;
          }
          else if (dn == "bpkg")
          {
            if (pkg != nullptr && d.constraint && !satisfy_bpkg (o, d))
            {
              fail << "unable to satisfy constraint (" << d << ") for "
                   << "package " << *pkg <<
                info << "available bpkg version is " << bpkg_version;
            }

            return true;
          }
        }
      }
    }

    return false;
  }

  bool
  has_dependencies (const common_options& o,
                    const dependencies& deps,
                    const package_name* pkg)
  {
    for (const auto& das: deps)
    {
      if (!toolchain_buildtime_dependency (o, das, pkg))
        return true;
    }

    return false;
  }
}
