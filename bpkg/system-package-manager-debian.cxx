// file      : bpkg/system-package-manager-debian.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager-debian.hxx>

#include <bpkg/diagnostics.hxx>

namespace bpkg
{
  // Do we use apt or apt-get? From apt(8):
  //
  // "The apt(8) commandline is designed as an end-user tool and it may change
  //  behavior between versions. [...]
  //
  //  All features of apt(8) are available in dedicated APT tools like
  //  apt-get(8) and apt-cache(8) as well. [...] So you should prefer using
  //  these commands (potentially with some additional options enabled) in
  //  your scripts as they keep backward compatibility as much as possible."

  // @@ We actually need to fetch if some are not installed to get their
  //    versions. We can do it as part of the call, no? Keep track if
  //    already fetched.

  // @@ We may map multiple our packages to the same system package
  //    (e.g., openssl-devel) so probably should track the status of
  //    individual system packages. What if we "installed any version"
  //    first and then need to install specific?


  // For background, a library in Debian is normally split up into several
  // packages: the shared library package (e.g., libfoo1 where 1 is the ABI
  // version), the development files package (e.g., libfoo-dev), the
  // documentation files package (e.g., libfoo-doc), the debug symbols
  // package (e.g., libfoo1-dbg), and the architecture-independent files
  // (e.g., libfoo1-common). All the packages except -dev are optional
  // and there is quite a bit of variability here. Here are a few examples:
  //
  // libz3-4 libz3-dev
  //
  // libssl1.1 libssl-dev libssl-doc
  // libssl3 libssl-dev libssl-doc
  //
  // libcurl4 libcurl4-doc libcurl4-openssl-dev
  // libcurl3-gnutls libcurl4-gnutls-dev
  //
  // Based on that, it seems our best bet when trying to automatically map our
  // library package name to Debian package names is to go for the -dev
  // package first and figure out the shared library package from that based
  // on the fact that the -dev package should have the == dependency on the
  // shared library package with the same version and its name should normally
  // start with the -dev package's stem.
  //
  // For a manual mapping we will require the user to always specify the
  // shared library package and the -dev package names explicitly.
  //
  // For executable packages there is normally no -dev packages but -dbg,
  // -doc, and -common are plausible.
  //
  class system_package_status_debian: public system_package_status
  {
  public:
    string main;
    string dev;
    string doc;
    string dbg;
    string common;
    strings extras;

    explicit
    system_package_status_debian (string m, string d = {})
        : main (move (m)), dev (move (d))
    {
      assert (!main.empty () || !dev.empty ());
    }
  };

  using package_status = system_package_status;
  using package_status_debian = system_package_status_debian;

  const package_status_debian&
  as_debian (const unique_ptr<package_status>& s)
  {
    return static_cast<const package_status_debian&> (*s);
  }

  package_status_debian&
  as_debian (unique_ptr<package_status>& s)
  {
    return static_cast<package_status_debian&> (*s);
  }

  // Parse the debian-name (or alike) value.
  //
  // The format of this value is a comma-separated list of one or more package
  // groups:
  //
  // <package-group> [, <package-group>...]
  //
  // Where each <package-group> is the space-separate list of one or more
  // package names:
  //
  // <package-name> [  <package-name>...]
  //
  // All the packages in the group should be "package components" (for the
  // lack of a better term) of the same "logical package", such as -dev, -doc,
  // -common packages. They usually have the same version.
  //
  // The first group is called the main group and the first package in the
  // group is called the main package.
  //
  // We allow/recommend specifying the -dev package as the main package for
  // libraries (the name starts with lib), seeing that we will be capable of
  // detecting the main package automatically. If the library name happens to
  // end with -dev (which poses an ambiguity), then the -dev package should be
  // specified explicitly as the second package to disambiguate this situation
  // (if a non-library name happened to start with lib and end with -dev,
  // well, you are out of luck, I guess).
  //
  // Note also that for now we treat all the packages from the non-main groups
  // as extras. But in the future we may decide to sort them out like the main
  // group.
  //
  static unique_ptr<package_status_debian>
  parse_debian_name (const string& nv)
  {
    auto split = [] (const string& s, char d) -> strings
    {
      strings r;
      for (size_t b (0), e (0); next_word (s, b, e, d); )
        r.push_back (string (s, b, e - b));
      return r;
    };

    auto suffix = [] (const string& n, const string& s) -> bool
    {
      size_t nn (n.size ());
      size_t sn (s.size ());
      return nn > sn && n.compare (nn - sn, sn, s) == 0;
    };

    auto parse_group = [&split, &suffix] (const string& g)
    {
      strings ns (split (g, ' '));

      if (ns.empty ())
        fail << "empty package group";

      unique_ptr<package_status_debian> r;

      // Handle the dev instead of main special case for libraries.
      //
      // Check that the following name does not end with -dev. This will be
      // the only way to disambiguate the case where the library name happens
      // to end with -dev (e.g., libops-dev libops-dev-dev).
      //
      {
        string& m (ns[0]);

        if (m.compare (0, 3, "lib") == 0 &&
            suffix (m, "-dev")           &&
            !(ns.size () > 1 && suffix (ns[1], "-dev")))
        {
          r.reset (new package_status_debian ("", move (m)));
        }
        else
          r.reset (new package_status_debian (move (m)));
      }

      // Handle the rest.
      //
      for (size_t i (1); i != ns.size (); ++i)
      {
        string& n (ns[i]);

        const char* w;
        if (string* v = (suffix (n, (w = "-dev"))    ? &r->dev :
                         suffix (n, (w = "-doc"))    ? &r->doc :
                         suffix (n, (w = "-dbg"))    ? &r->dbg :
                         suffix (n, (w = "-common")) ? &r->common : nullptr))
        {
          if (!v->empty ())
            fail << "multiple " << w << " package names in '" << g << "'" <<
              info << "did you forget to separate package groups with comma?";

          *v = move (n);
        }
        else
          r->extras.push_back (move (n));
      }

      return r;
    };

    strings gs (split (nv, ','));
    assert (!gs.empty ()); // *-name value cannot be empty.

    unique_ptr<package_status_debian> r;
    for (size_t i (0); i != gs.size (); ++i)
    {
      if (i == 0) // Main group.
        r = parse_group (gs[i]);
      else
      {
        unique_ptr<package_status_debian> g (parse_group (gs[i]));

        // @@ Shouldn't we filter some based on what we are installing?

        if (!g->main.empty ())   r->extras.push_back (move (g->main));
        if (!g->dev.empty ())    r->extras.push_back (move (g->dev));
        if (!g->doc.empty ())    r->extras.push_back (move (g->doc));
        if (!g->dbg.empty ())    r->extras.push_back (move (g->dbg));
        if (!g->common.empty ()) r->extras.push_back (move (g->common));
        if (!g->extras.empty ()) r->extras.insert (
          r->extras.end (),
          make_move_iterator (g->extras.begin ()),
          make_move_iterator (g->extras.end ()));
      }
    }

    return r;
  }

  optional<const system_package_status*>
  system_package_manager_debian::
  pkg_status (const package_name& pn,
              const available_packages* aps,
              bool install,
              bool fetch)
  {
    // First check the cache.
    //
    {
      auto i (status_cache_.find (pn));

      if (i != status_cache_.end ())
        return i->second.get ();

      if (aps == nullptr)
        return nullptr;
    }

    vector<unique_ptr<package_status_debian>> rs;

    // Translate our package name to the Debian package names.
    //
    {
      auto df = make_diag_frame (
        [&pn] (const diag_record& dr)
        {
          dr << info << "while mapping " << pn << " to Debian package name";
        });

      strings ns (system_package_names (*aps,
                                        os_release_.name_id,
                                        os_release_.version_id,
                                        os_release_.like_ids));
      if (ns.empty ())
      {
        // Attempt to automatically translate our package name (see above for
        // details).
        //
        const string& n (pn.string ());

        // The best we can do in trying to detect whether this is a library is
        // to check for the lib prefix. Libraries without the lib prefix and
        // non-libraries with the lib prefix (both of which we do not
        // recomment) will have to provide a manual mapping.
        //
        unique_ptr<package_status_debian> s;

        if (n.compare (0, 3, "lib") == 0)
        {
          // Keep the main package name empty as an indication that it is to
          // be discovered.
          //
          s.reset (new package_status_debian ("", n + "-dev"));
        }
        else
          s.reset (new package_status_debian (n));

        rs.push_back (move (s));
      }
      else
      {
        // Parse each manual mapping.
        //
        for (const string& n: ns)
        {
          unique_ptr<package_status_debian> s (parse_debian_name (n));

          // Suppress duplicates for good measure based on the main package
          // name (and falling back to -dev if empty).
          //
          auto i (find_if (rs.begin (), rs.end (),
                           [&s] (const unique_ptr<package_status_debian>& x)
                           {
                             return s->main.empty ()
                               ? s->dev == x->dev
                               : s->main == x->main;
                           }));
          if (i == rs.end ())
            rs.push_back (move (s));
          else
          {
            // @@ Should we verify the rest matches for good measure?
          }
        }
      }
    }

    // First look for an already installed package.
    //
    unique_ptr<package_status_debian> r;


    // Next look for available versions if we are allowed to install.
    //
    if (r == nullptr && install)
    {
      if (fetch && !fetched_)
      {
        // @@ TODO: apt-get update

        fetched_ = true;
      }
    }

    // Cache.
    //
    return status_cache_.emplace (pn, move (r)).first->second.get ();
  }

  void system_package_manager_debian::
  pkg_install (const vector<package_name>&)
  {
    assert (!installed_);
    installed_ = true;
  }
}
