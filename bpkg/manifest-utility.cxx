// file      : bpkg/manifest-utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/manifest-utility.hxx>

#include <sstream>
#include <cstring> // strcspn()

#include <libbutl/b.hxx>
#include <libbutl/filesystem.hxx>  // dir_iterator

#include <bpkg/package.hxx>        // wildcard_version
#include <bpkg/diagnostics.hxx>
#include <bpkg/common-options.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  const path repositories_file ("repositories.manifest");
  const path packages_file     ("packages.manifest");
  const path signature_file    ("signature.manifest");
  const path manifest_file     ("manifest");

  vector<package_info>
  package_b_info (const common_options& o,
                  const dir_paths& ds,
                  b_info_flags fl)
  {
    path b (name_b (o));

    vector<package_info> r;
    try
    {
      b_info (r,
              ds,
              fl,
              verb,
              [] (const char* const args[], size_t n)
              {
                if (verb >= 2)
                  print_process (args, n);
              },
              b,
              exec_dir,
              o.build_option ());
      return r;
    }
    catch (const b_error& e)
    {
      if (e.normal ())
        throw failed (); // Assume the build2 process issued diagnostics.

      diag_record dr (fail);
      dr << "unable to parse project ";
      if (r.size () < ds.size ()) dr << ds[r.size ()] << ' ';
      dr << "info: " << e <<
        info << "produced by '" << b << "'; use --build to override" << endf;

      return vector<package_info> (); // Work around GCC 13.2.1 segfault.
    }
  }

  package_scheme
  parse_package_scheme (const char*& s)
  {
    // Ignore the character case for consistency with a case insensitivity of
    // URI schemes some of which we may support in the future.
    //
    if (icasecmp (s, "sys:", 4) == 0)
    {
      s += 4;
      return package_scheme::sys;
    }

    return package_scheme::none;
  }

  package_name
  parse_package_name (const char* s, bool allow_version)
  {
    try
    {
      return extract_package_name (s, allow_version);
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid package name " << (allow_version ? "in " : "")
           << "'" << s << "': " << e << endf;
    }
  }

  version
  parse_package_version (const char* s,
                         bool allow_wildcard,
                         version::flags fl)
  {
    using traits = string::traits_type;

    if (const char* p = traits::find (s, traits::length (s), '/'))
    {
      if (*++p == '\0')
        fail << "empty package version in '" << s << "'";

      if (allow_wildcard && strcmp (p, "*") == 0)
        return wildcard_version;

      try
      {
        return extract_package_version (s, fl);
      }
      catch (const invalid_argument& e)
      {
        fail << "invalid package version '" << p << "' in '" << s << "': "
             << e;
      }
    }

    return version ();
  }

  optional<version_constraint>
  parse_package_version_constraint (const char* s,
                                    bool allow_wildcard,
                                    version::flags fl,
                                    bool version_only)
  {
    // Calculate the version specification position as a length of the prefix
    // that doesn't contain slashes and the version constraint starting
    // characters.
    //
    size_t n (strcspn (s, "/=<>([~^"));

    if (s[n] == '\0') // No version (constraint) is specified?
      return nullopt;

    const char* v (s + n); // Constraint or version including leading '/'.

    if (version_only && v[0] != '/')
      fail << "exact package version expected instead of version constraint "
           << "in '" << s << "'";

    // If the package name is followed by '/' then fallback to the version
    // parsing.
    //
    if (v[0] == '/')
    try
    {
      return version_constraint (
        parse_package_version (s, allow_wildcard, fl));
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid package version '" << v + 1 << "' in '" << s << "': "
           << e;
    }

    try
    {
      version_constraint r (v);

      if (!r.complete ())
        throw invalid_argument ("incomplete");

      // There doesn't seem to be any good reason to allow specifying a stub
      // version in the version constraint. Note that the constraint having
      // both endpoints set to the wildcard version (which is a stub) denotes
      // the system package wildcard version and may result only from the '/*'
      // string representation.
      //
      auto stub = [] (const optional<version>& v)
      {
        return v && v->compare (wildcard_version, true) == 0;
      };

      if (stub (r.min_version) || stub (r.max_version))
        throw invalid_argument ("endpoint is a stub");

      return r;
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid package version constraint '" << v << "' in '" << s
           << "': " << e << endf;
    }
  }

  repository_location
  parse_location (const string& s, optional<repository_type> ot)
  try
  {
    typed_repository_url tu (s);

    repository_url& u (tu.url);
    assert (u.path);

    // Make the relative path absolute using the current directory.
    //
    if (u.scheme == repository_protocol::file && u.path->relative ())
      u.path->complete ().normalize ();

    // Guess the repository type to construct the repository location:
    //
    // 1. If the type is specified in the URL scheme, then use that (but
    //    validate that it matches the --type option, if present).
    //
    // 2. If the type is specified as an option, then use that.
    //
    // Validate the protocol/type compatibility (e.g. git:// vs pkg) for both
    // cases.
    //
    // 3. See the guess_type() function description in <libbpkg/manifest.hxx>
    //    for the algorithm details.
    //
    if (tu.type && ot && tu.type != ot)
      fail << to_string (*ot) << " repository type mismatch for location '"
           << s << "'";

    repository_type t (tu.type ? *tu.type :
                       ot      ? *ot      :
                       guess_type (tu.url, true /* local */));

    try
    {
      // Don't move the URL since it may still be needed for diagnostics.
      //
      return repository_location (u, t);
    }
    catch (const invalid_argument& e)
    {
      diag_record dr;
      dr << fail << "invalid " << t << " repository location '" << u << "': "
         << e;

      // If the pkg repository type was guessed, then suggest the user to
      // specify the type explicitly.
      //
      if (!tu.type && !ot && t == repository_type::pkg)
        dr << info << "consider using --type to specify repository type";

      dr << endf;
    }
  }
  catch (const invalid_path& e)
  {
    fail << "invalid repository path '" << s << "': " << e << endf;
  }
  catch (const invalid_argument& e)
  {
    fail << "invalid repository location '" << s << "': " << e << endf;
  }
  catch (const system_error& e)
  {
    fail << "failed to guess repository type for '" << s << "': " << e << endf;
  }

  dir_path
  repository_state (const repository_location& rl)
  {
    switch (rl.type ())
    {
    case repository_type::pkg:
    case repository_type::dir: return dir_path (); // No state.

    case repository_type::git:
      {
        // Strip the fragment, so all the repository fragments of the same
        // git repository can reuse the state. So, for example, the state is
        // shared for the fragments fetched from the following git repository
        // locations:
        //
        // https://www.example.com/foo.git#master
        // git://example.com/foo#stable
        //
        repository_url u (rl.url ());
        u.fragment = nullopt;

        repository_location l (u, rl.type ());
        return dir_path (sha256 (l.canonical_name ()).abbreviated_string (12));
      }
    }

    assert (false); // Can't be here.
    return dir_path ();
  }

  bool
  repository_name (const string& s)
  {
    size_t p (s.find (':'));

    // If it has no scheme, then this is not a canonical name.
    //
    if (p == string::npos)
      return false;

    // This is a canonical name if the scheme is convertible to the repository
    // type and is followed by the colon and no more than one slash.
    //
    // Note that the approach is valid unless we invent the file scheme for the
    // canonical name.
    //
    try
    {
      string scheme (s, 0, p);
      to_repository_type (scheme);
      bool r (!(p + 2 < s.size () && s[p + 1] == '/' && s[p + 2] == '/'));

      assert (!r || scheme != "file");
      return r;
    }
    catch (const invalid_argument&)
    {
      return false;
    }
  }

  package_version_infos
  package_versions (const common_options& o,
                    const dir_paths& ds,
                    b_info_flags fl)
  {
    vector<b_project_info> pis (package_b_info (o, ds, fl));

    package_version_infos r;
    r.reserve (pis.size ());

    for (const b_project_info& pi: pis)
    {
      // An empty version indicates that the version module is not enabled for
      // the project.
      //
      optional<version> v (!pi.version.empty ()
                           ? version (pi.version.string ())
                           : optional<version> ());

      r.push_back (package_version_info {move (v), move (pi)});
    }

    return r;
  }

  string
  package_checksum (const common_options& o,
                    const dir_path& d,
                    const package_info* pi)
  {
    path f (d / manifest_file);

    try
    {
      ifdstream is (f, fdopen_mode::binary);
      sha256 cs (is);

      const vector<package_info::subproject>& sps (
        pi != nullptr
        ? pi->subprojects
        : package_b_info (o, d, b_info_flags::subprojects).subprojects);

      for (const package_info::subproject& sp: sps)
        cs.append (sp.path.string ());

      return cs.string ();
    }
    catch (const io_error& e)
    {
      fail << "unable to read from " << f << ": " << e << endf;
    }
  }

  // Return the sorted list of *.build files (first) which are present in the
  // package's build/config/ subdirectory (or their alternatives) together
  // with the *-build manifest value names they correspond to (second). Skip
  // files which are already present in the specified buildfile/path
  // lists. Note: throws system_error on filesystem errors.
  //
  static vector<pair<path, path>>
  find_buildfiles (const dir_path& config,
                   const string& ext,
                   const vector<buildfile>& bs,
                   const vector<path>& bps)
  {
    vector<pair<path, path>> r;

    for (const dir_entry& de: dir_iterator (config, dir_iterator::no_follow))
    {
      if (de.type () == entry_type::regular)
      {
        const path& p (de.path ());
        const char* e (p.extension_cstring ());

        if (e != nullptr && ext == e)
        {
          path f (config.leaf () / p.base ()); // Relative to build/.

          if (find_if (bs.begin (), bs.end (),
                       [&f] (const auto& v) {return v.path == f;}) ==
              bs.end () &&
              find (bps.begin (), bps.end (), f) == bps.end ())
          {
            r.emplace_back (config / p, move (f));
          }
        }
      }
    }

    sort (r.begin (), r.end (),
          [] (const auto& x, const auto& y) {return x.second < y.second;});

    return r;
  }

  string
  package_buildfiles_checksum (const optional<string>& bb,
                               const optional<string>& rb,
                               const vector<buildfile>& bs,
                               const dir_path& d,
                               const vector<path>& bps,
                               optional<bool> an)
  {
    if (d.empty ())
    {
      assert (bb);

      sha256 cs (*bb);

      if (rb)
        cs.append (*rb);

      for (const buildfile& b: bs)
        cs.append (b.content);

      return cs.string ();
    }

    auto checksum = [&bb, &rb, &bs, &bps] (const path& b,
                                           const path& r,
                                           const dir_path& c,
                                           const string& e)
    {
      sha256 cs;

      auto append_file = [&cs] (const path& f)
      {
        try
        {
          // Open the buildfile in the text mode and hash the NULL character
          // at the end to calculate the checksum over files consistently with
          // calculating it over the *-build manifest values.
          //
          ifdstream ifs (f);
          cs.append (ifs);
          cs.append ('\0');
        }
        catch (const io_error& e)
        {
          fail << "unable to read from " << f << ": " << e;
        }
      };

      if (bb)
        cs.append (*bb);
      else
        append_file (b);

      bool root (true);

      if (rb)
        cs.append (*rb);
      else if (exists (r))
        append_file (r);
      else
        root = false;

      for (const buildfile& b: bs)
        cs.append (b.content);

      if (!bps.empty ())
      {
        dir_path bd (b.directory ());

        for (const path& p: bps)
        {
          path f (bd / p);
          f += '.' + e;

          append_file (f);
        }
      }

      if (root && exists (c))
      try
      {
        for (auto& f: find_buildfiles (c, e, bs, bps))
          append_file (f.first);
      }
      catch (const system_error& e)
      {
        fail << "unable to scan directory " << c << ": " << e;
      }

      return string (cs.string ());
    };

    // Verify that the deduced naming scheme matches the specified one and
    // fail if that's not the case.
    //
    auto verify = [an, &d] (bool alt_naming)
    {
      assert (an);

      if (*an != alt_naming)
        fail << "buildfile naming scheme mismatch between manifest and "
             << "package directory " << d;
    };

    // Check the alternative bootstrap file first since it is more specific.
    //
    path bf;
    if (exists (bf = d / alt_bootstrap_file))
    {
      if (an)
        verify (true /* alt_naming */);

      return checksum (bf,
                       d / alt_root_file,
                       d / alt_config_dir,
                       alt_build_ext);
    }
    else if (exists (bf = d / std_bootstrap_file))
    {
      if (an)
        verify (false /* alt_naming */);

      return checksum (bf,
                       d / std_root_file,
                       d / std_config_dir,
                       std_build_ext);
    }
    else
      fail << "unable to find bootstrap.build file in package directory "
           << d << endf;
  }

  void
  load_package_buildfiles (package_manifest& m, const dir_path& d, bool erp)
  {
    assert (m.buildfile_paths.empty ()); // build-file values must be expanded.

    auto load_buildfiles = [&m, &d, erp] (const path& b,
                                          const path& r,
                                          const dir_path& c,
                                          const string& ext)
    {
      auto diag_path = [&d, erp] (const path& p)
      {
        return !erp ? p : p.leaf (d);
      };

      auto load = [&diag_path] (const path& f)
      {
        try
        {
          ifdstream ifs (f);
          string r (ifs.read_text ());
          ifs.close ();
          return r;
        }
        catch (const io_error& e)
        {
          // Sanitize the exception description.
          //
          ostringstream os;
          os << "unable to read from " << diag_path (f) << ": " << e;
          throw runtime_error (os.str ());
        }
      };

      if (!m.bootstrap_build)
        m.bootstrap_build = load (b);

      if (!m.root_build && exists (r))
        m.root_build = load (r);

      if (m.root_build && exists (c))
      try
      {
        for (auto& f: find_buildfiles (c,
                                       ext,
                                       m.buildfiles,
                                       m.buildfile_paths))
        {
          m.buildfiles.emplace_back (move (f.second), load (f.first));
        }
      }
      catch (const system_error& e)
      {
        // Sanitize the exception description.
        //
        ostringstream os;
        os << "unable to scan directory " << diag_path (c) << ": " << e;
        throw runtime_error (os.str ());
      }
    };

    // Set the manifest's alt_naming flag to the deduced value if absent and
    // verify that it matches otherwise.
    //
    auto alt_naming = [&m, &d, erp] (bool v)
    {
      if (!m.alt_naming)
      {
        m.alt_naming = v;
      }
      else if (*m.alt_naming != v)
      {
        string e ("buildfile naming scheme mismatch between manifest and "
                  "package directory");

        if (!erp)
          e += ' ' + d.string ();

        throw runtime_error (e);
      }
    };

    // Check the alternative bootstrap file first since it is more specific.
    //
    path bf;
    if (exists (bf = d / alt_bootstrap_file))
    {
      alt_naming (true);

      load_buildfiles (bf,
                       d / alt_root_file,
                       d / alt_config_dir,
                       alt_build_ext);
    }
    else if (exists (bf = d / std_bootstrap_file))
    {
      alt_naming (false);

      load_buildfiles (bf,
                       d / std_root_file,
                       d / std_config_dir,
                       std_build_ext);
    }
    else
    {
      string e ("unable to find bootstrap.build file in package directory");

      if (!erp)
        e += ' ' + d.string ();

      throw runtime_error (e);
    }
  }
}
