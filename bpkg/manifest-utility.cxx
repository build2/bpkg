// file      : bpkg/manifest-utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/manifest-utility.hxx>

#include <libbutl/url.mxx>
#include <libbutl/sha256.mxx>

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
    fdpipe pipe (open_pipe ());

    process pr (start_b (o,
                         pipe, 2 /* stderr */,
                         verb_b::quiet,
                         "info:",
                         d.representation ()));

    // Shouldn't throw, unless something is severely damaged.
    //
    pipe.out.close ();

    try
    {
      optional<version> r;

      ifdstream is (move (pipe.in),
                    fdstream_mode::skip,
                    ifdstream::badbit);

      for (string l; !eof (getline (is, l)); )
      {
        if (l.compare (0, 9, "version: ") == 0)
        try
        {
          string v (l, 9);

          // An empty version indicates that the version module is not
          // enabled for the project.
          //
          if (!v.empty ())
            r = version (v);

          break;
        }
        catch (const invalid_argument&)
        {
          fail << "no package version in '" << l << "'" <<
            info << "produced by '" << name_b (o) << "'; use --build to "
                 << "override";
        }
      }

      is.close ();

      if (pr.wait ())
        return r;

      // Fall through.
    }
    catch (const io_error&)
    {
      if (pr.wait ())
        fail << "unable to read '" << name_b (o) << "' output";

      // Fall through.
    }

    // We should only get here if the child exited with an error status.
    //
    assert (!pr.wait ());

    fail << "unable to obtain version using '" << name_b (o) << "'" << endf;
  }
}
