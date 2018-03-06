// file      : bpkg/manifest-utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/manifest-utility.hxx>

#include <libbutl/url.mxx>
#include <libbutl/sha256.mxx>

#include <bpkg/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  const path repositories_file ("repositories.manifest");
  const path packages_file     ("packages.manifest");
  const path signature_file    ("signature.manifest");

  package_scheme
  parse_package_scheme (const char*& s)
  {
    // Ignore the character case for consistency with a case insensitivity of
    // URI schemes some of which we may support in the future.
    //
    if (casecmp (s, "sys:", 4) == 0)
    {
      s += 4;
      return package_scheme::sys;
    }

    return package_scheme::none;
  }

  string
  parse_package_name (const char* s)
  {
    using traits = string::traits_type;

    size_t n (traits::length (s));

    if (const char* p = traits::find (s, n, '/'))
      n = static_cast<size_t> (p - s);

    if (n == 0)
      fail << "empty package name in '" << s << "'";

    return string (s, n);
  }

  version
  parse_package_version (const char* s)
  {
    using traits = string::traits_type;

    if (const char* p = traits::find (s, traits::length (s), '/'))
    {
      if (*++p == '\0')
        fail << "empty package version in '" << s << "'";

      try
      {
        return version (p);
      }
      catch (const invalid_argument& e)
      {
        fail << "invalid package version '" << p << "': " << e;
      }
    }

    return version ();
  }

  repository_location
  parse_location (const string& s, optional<repository_type> ot)
  try
  {
    repository_url u (s);

    if (u.empty ())
      fail << "empty repository location";

    assert (u.path);

    // Make the relative path absolute using the current directory.
    //
    if (u.scheme == repository_protocol::file && u.path->relative ())
      u.path->complete ().normalize ();

    // Guess the repository type to construct the repository location:
    //
    // 1. If type is specified as an option use that (but validate
    //    incompatible scheme/type e.g., git/pkg).
    //
    // 2. See guess_type() function description in libbpkg/manifest.hxx for
    //    the algorithm details.
    //
    repository_type t (ot ? *ot : guess_type (u, true));

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
      if (!ot && t == repository_type::pkg)
        dr << info << "consider using --type to specify repository type";

      dr << endf;
    }
  }
  catch (const invalid_argument& e)
  {
    fail << "invalid repository location '" << s << "': " << e << endf;
  }
  catch (const invalid_path& e)
  {
    fail << "invalid repository path '" << s << "': " << e << endf;
  }
  catch (const system_error& e)
  {
    fail << "failed to guess repository type for '" << s << "': " << e << endf;
  }

  dir_path
  repository_state (const repository_location& l)
  {
    switch (l.type ())
    {
    case repository_type::pkg:
    case repository_type::dir: return dir_path (); // No state.

    case repository_type::git:
      {
        return dir_path (sha256 (l.canonical_name ()).abbreviated_string (16));
      }
    }

    assert (false); // Can't be here.
    return dir_path ();
  }

  bool
  repository_name (const string& s)
  {
    size_t p (s.find (':'));

    // If it has no scheme or the scheme looks like that of a URL, then this
    // is not a canonical name.
    //
    if (p == string::npos || butl::url::traits::find (s, p) != string::npos)
      return false;

    // This is a canonical name if the scheme is convertible to the repository
    // type.
    //
    try
    {
      to_repository_type (string (s, 0, p));
      return true;
    }
    catch (const invalid_argument&)
    {
      return false;
    }
  }
}
