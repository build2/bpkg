// file      : bpkg/manifest-utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/manifest-utility.hxx>

#include <cstring> // strcspn()

#include <libbutl/b.mxx>
#include <libbutl/sha256.mxx>

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
    if (!allow_version)
    try
    {
      return package_name (s);
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid package name '" << s << "': " << e;
    }

    // Calculate the package name length as a length of the prefix that
    // doesn't contain spaces, slashes and the version constraint starting
    // characters. Note that none of them are valid package name characters.
    //
    size_t n (strcspn (s, " /=<>([~^"));

    try
    {
      return package_name (string (s, n));
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid package name in '" << s << "': " << e << endf;
    }
  }

  version
  parse_package_version (const char* s,
                         bool allow_wildcard,
                         bool fold_zero_revision)
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
        version r (p, fold_zero_revision);

        if (r.release && r.release->empty ())
          throw invalid_argument ("earliest version");

        if (r.compare (wildcard_version, true /* ignore_revision */) == 0)
          throw invalid_argument ("stub version");

        return r;
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
                                    bool fold_zero_revision,
                                    bool version_only)
  {
    // Calculate the version specification position as a length of the prefix
    // that doesn't contain slashes and the version constraint starting
    // characters.
    //
    size_t n (strcspn (s, "/=<>([~^"));

    if (s[n] == '\0') // No version (constraint) is specified?
      return nullopt;

    const char* v (s + n); // Constraint or version including '/'.

    // If only the version is allowed or the package name is followed by '/'
    // then fallback to the version parsing.
    //
    if (version_only || v[0] == '/')
    try
    {
      return version_constraint (
        parse_package_version (s, allow_wildcard, fold_zero_revision));
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

  optional<version>
  package_version (const common_options& o, const dir_path& d)
  {
    path b (name_b (o));

    try
    {
      b_project_info pi (
        b_info (d,
                verb,
                [] (const char* const args[], size_t n)
                {
                  if (verb >= 2)
                    print_process (args, n);
                },
                b,
                exec_dir,
                o.build_option ()));

      optional<version> r;

      // An empty version indicates that the version module is not enabled for
      // the project.
      //
      if (!pi.version.empty ())
        r = version (pi.version.string ());

      return r;
    }
    catch (const b_error& e)
    {
      if (e.normal ())
        throw failed (); // Assume the build2 process issued diagnostics.

      fail << "unable to parse project " << d << " info: " << e <<
        info << "produced by '" << b << "'; use --build to override" << endf;
    }
  }
}
