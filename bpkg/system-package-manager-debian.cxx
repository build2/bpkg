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
  // libcurl3-gnutls libcurl4-gnutls-dev         (yes, 3 and 4)
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
  struct package_policy // apt-cache policy output
  {
    reference_wrapper<const string> name;

    string installed_version; // Empty if none.
    string candidate_version; // Empty if none.

    explicit
    package_policy (const string& n): name (n) {}
  };

  class system_package_status_debian: public system_package_status
  {
  public:
    string main;
    string dev;
    string doc;
    string dbg;
    string common;
    strings extras;

    vector<package_policy> package_policies;

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
  // as extras (but in the future we may decide to sort them out like the main
  // group). For now we omit the -common package (assuming it's pulled by the
  // main package) as well as -doc and -dbg unless requested (the
  // extra_{doc,dbg} arguments).
  //
  static unique_ptr<package_status_debian>
  parse_debian_name (const string& nv,
                     bool extra_doc,
                     bool extra_dbg)
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

        if (!g->main.empty ())             r->extras.push_back (move (g->main));
        if (!g->dev.empty ())              r->extras.push_back (move (g->dev));
        if (!g->doc.empty () && extra_doc) r->extras.push_back (move (g->doc));
        if (!g->dbg.empty () && extra_dbg) r->extras.push_back (move (g->dbg));
        if (!g->common.empty () && false)  r->extras.push_back (move (g->common));
        if (!g->extras.empty ())           r->extras.insert (
          r->extras.end (),
          make_move_iterator (g->extras.begin ()),
          make_move_iterator (g->extras.end ()));
      }
    }

    return r;
  }

  static process_path apt_cache;

  // Obtain the installed and candidate versions for the specified list
  // of Debian packages by executing apt-cache policy.
  //
  // If the n argument is not 0, then only query the first n packages.
  //
  static void
  apt_cache_policy (vector<package_policy>& pps, size_t n = 0)
  {
    if (n == 0)
      n = pps.size ();

    assert (n != 0 && n <= pps.size ());

    // In particular, --quite makes sure we don't get a noice (N) printed to
    // stderr if the package is unknown. It does not appear to affect error
    // diagnostics (try temporarily renaming /var/lib/dpkg/status).
    //
    cstrings args {"apt-cache", "policy", "--quiet"};

    for (size_t i (0); i != n; ++i)
    {
      package_policy& pp (pps[i]);

      const string& n (pp.name);
      assert (!n.empty ());

      pp.installed_version.clear ();
      pp.candidate_version.clear ();

      args.push_back (n.c_str ());
    }

    args.push_back (nullptr);

    // Run with the C locale to make sure there is no localization. Note that
    // this is not without potential drawbacks, see Debian bug #643787. But
    // for now it seems to work and feels like the least of two potential
    // evils.
    //
    const char* evars[] = {"LC_ALL=C", nullptr};

    try
    {
      if (apt_cache.empty ())
        apt_cache = process::path_search (args[0]);

      process_env pe (apt_cache, evars);

      if (verb >= 3)
        print_process (pe, args);

      // Redirect stdout to a pipe. For good measure also redirect stdin to
      // /dev/null to make sure there are no prompts of any kind.
      //
      process pr (apt_cache,
                  args,
                  -2      /* stdin */,
                  -1      /* stdout */,
                  2       /* stderr */,
                  nullptr /* cwd */,
                  evars);
      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

        // The output of `apt-cache policy <pkg1> <pkg2> ...` are blocks of
        // lines in the following form:
        //
        // <pkg1>:
        //   Installed: 1.2.3-1
        //   Candidate: 1.3.0-2
        //   Version table:
        //     <...>
        // <pkg2>:
        //   Installed: (none)
        //   Candidate: 1.3.0+dfsg-2+b1
        //   Version table:
        //     <...>
        //
        // Where <...> are further lines indented with at least one space. If
        // a package is unknown, then the entire block (including the first
        // <pkg>: line) is omitted. The blocks appear in the same order as
        // packages on the command line and multiple entries for the same
        // package result in multiple corresponding blocks. It looks like
        // there should be not blank lines but who really knows.
        //
        {
          auto df = make_diag_frame (
            [&pe, &args] (diag_record& dr)
            {
              dr << info << "while parsing output of ";
              print_process (dr, pe, args);
            });

          size_t i (0);

          string l;
          for (getline (is, l); !eof (is); )
          {
            // Parse the first line of the block.
            //
            if (l.empty () || l.front () == ' ' || l.back () != ':')
              fail << "expected package name instead of '" << l << "'";

            l.pop_back ();

            // Skip until this package.
            //
            for (; i != n && pps[i].name.get () != l; ++i) ;

            if (i == n)
              fail << "unexpected package name '" << l << "'";

            package_policy& pp (pps[i]);

            auto parse_version = [&l] (const string& n) -> string
            {
              size_t s (n.size ());

              if (l[0] == ' '              &&
                  l[1] == ' '              &&
                  l.compare (2, s, n) == 0 &&
                  l[2 + s] == ':')
              {
                string v (l, 2 + s + 1);
                trim (v);

                if (!v.empty ())
                  return v == "(none)" ? string () : move (v);
              }

              fail << "invalid " << n << " version line '" << l << "'" << endf;
            };

            // Get the installed version line.
            //
            if (eof (getline (is, l)))
              fail << "expected Installed version line after package name";

            pp.installed_version = parse_version ("Installed");

            // Get the candidate version line.
            //
            if (eof (getline (is, l)))
              fail << "expected Candidate version line after Installed version";

            pp.candidate_version = parse_version ("Candidate");

            // Skip the rest of the indented lines (or blanks, just in case).
            //
            while (!eof (getline (is, l)) && (l.empty () || l.front () != ' ')) ;
          }
        }

        is.close ();
      }
      catch (const io_error& e)
      {
        if (pr.wait ())
          fail << "unable to read " << args[0] << " policy output: " << e;

        // Fall through.
      }

      if (!pr.wait ())
      {
        diag_record dr (fail);
        dr << args[0] << " policy exited with non-zero code";

        if (verb < 3)
        {
          dr << "command line: ";
          print_process (dr, pe, args);
        }
      }
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  // Return the Depends value, if any, for the specified package and version.
  // Fail if either package or version is unknown.
  //
  static string
  apt_cache_show (const string& name, const string& ver)
  {
    assert (!name.empty () && !ver.empty ());

    string spec (name + '=' + ver);

    // In particular, --quite makes sure we don't get noices (N) printed to
    // stderr. It does not appear to affect error diagnostics (try showing
    // information for an unknown package).
    //
    const char* args[] = {
      "apt-cache", "show", "--quiet", spec.c_str (), nullptr};

    // Note that for this command there seems to be no need to run with the C
    // locale since the output is presumably not localizable. But let's do it
    // for good measure and also seeing that we try to backfit some
    // diagnostics into apt-cache (see no_version below).
    //
    const char* evars[] = {"LC_ALL=C", nullptr};

    string r;
    try
    {
      if (apt_cache.empty ())
        apt_cache = process::path_search (args[0]);

      process_env pe (apt_cache, evars);

      if (verb >= 3)
        print_process (pe, args);

      // Redirect stdout to a pipe. For good measure also redirect stdin to
      // /dev/null to make sure there are no prompts of any kind.
      //
      process pr (apt_cache,
                  args,
                  -2      /* stdin */,
                  -1      /* stdout */,
                  2       /* stderr */,
                  nullptr /* cwd */,
                  evars);

      bool no_version (false);
      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

        // The output of `apt-cache show <pkg>=<ver>` appears to be a single
        // Debian control file in the RFC 822 encoding followed by a blank
        // line. See deb822(5) for details. Here is a representative example:
        //
        // Package: libcurl4
        // Version: 7.85.0-1
        // Depends: libbrotli1 (>= 0.6.0), libc6 (>= 2.34), ...
        // Description-en: easy-to-use client-side URL transfer library
        //  libcurl is an easy-to-use client-side URL transfer library.
        //
        // Note that if the package is unknown, then we get an error but if
        // the version is unknown, we get no output (and a note if running
        // without --quiet).
        //
        string l;
        if (eof (getline (is, l)))
        {
          // The unknown version case. Issue diagnostics consistent with the
          // unknown package case, at least for the English locale.
          //
          text << "E: No package version found";
          no_version = true;
        }
        else
        {
          auto df = make_diag_frame (
            [&pe, &args] (diag_record& dr)
            {
              dr << info << "while parsing output of ";
              print_process (dr, pe, args);
            });

          do
          {
            // This line should be the start of a field unless it's a
            // comment. According to deb822(5), there can be no leading
            // whitespaces before `#`.
            //
            if (l[0] == '#')
            {
              getline (is, l);
              continue;
            }

            size_t p (l.find (':'));

            if (p == string::npos)
              fail << "expected field name instead of '" << l << "'";

            // Extract the field name. Note that field names are case-
            // insensitive.
            //
            string n (l, 0, p);
            trim (n);

            // Extract the field value.
            //
            string v (l, p + 1);
            trim (v);

            // If we have more lines see if the following line is part of this
            // value.
            //
            while (!eof (getline (is, l)) && (l[0] == ' ' || l[0] == '\t'))
            {
              // This can either be a "folded" or a "multiline" field and
              // which one it is depends on the field semantics. Here we only
              // care about Depends and so treat them all as folded (it's
              // unclear whether Depends must be a simple field).
              //
              trim (l);
              v += ' ';
              v += l;
            }

            // See if this is a field of interest.
            //
            if (icasecmp (n, "Package") == 0)
            {
              assert (v == name); // Sanity check.
            }
            else if (icasecmp (n, "Version") == 0)
            {
              assert (v == ver); // Sanity check.
            }
            else if (icasecmp (n, "Depends") == 0)
            {
              r = move (v);

              // Let's not waste time reading any further.
              //
              break;
            }
          }
          while (!eof (is));
        }

        is.close ();
      }
      catch (const io_error& e)
      {
        if (pr.wait ())
          fail << "unable to read " << args[0] << " policy output: " << e;

        // Fall through.
      }

      if (!pr.wait () || no_version)
      {
        diag_record dr (fail);
        dr << args[0] << " policy exited with non-zero code";

        if (verb < 3)
        {
          dr << "command line: ";
          print_process (dr, pe, args);
        }
      }
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }

    return r;
  }

  // Attempt to determine the main package name from its -dev package. Return
  // empty string if unable. Save the extracted Depends value to the depends
  // argument for diagnostics.
  //
  static string
  main_from_dev (const string& dev_name,
                 const string& dev_ver,
                 string& depends)
  {
    depends = apt_cache_show (dev_name, dev_ver);

    // The format of the Depends value is a comma-seperated list of dependency
    // expressions. For example:
    //
    // Depends: libssl3 (= 3.0.7-1), libc6 (>= 2.34), libfoo | libbar
    //
    // For the main package we look for a dependency in the form:
    //
    // <dev-stem>* (= <dev-ver>)
    //
    // Usually it is the first one.
    //
    string dev_stem (dev_name, 0, dev_name.rfind ("-dev"));

    string r;
    for (size_t b (0), e (0); next_word (depends, b, e, ','); )
    {
      string d (depends, b, e - b);
      trim (d);

      size_t p (d.find (' '));
      if (p != string::npos)
      {
        if (d.compare (0, dev_stem.size (), dev_stem) == 0) // <dev-stem>*
        {
          size_t q (d.find ('(', p + 1));
          if (q != string::npos && d.back () == ')') // (...)
          {
            if (d[q + 1] == '=' && d[q + 2] == ' ') // Equal.
            {
              string v (d, q + 3, d.size () - q - 3 - 1);
              trim (v);

              if (v == dev_ver)
              {
                r.assign (d, 0, p);
                break;
              }
            }
          }
        }
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
    // For now we ignore -doc and -dbg package components (but we may want to
    // have options controlling this later). Note also that we assume -common
    // is pulled automatically by the main package so we ignore it as well.
    //
    bool need_doc (false);
    bool need_dbg (false);

    // First check the cache.
    //
    {
      auto i (status_cache_.find (pn));

      if (i != status_cache_.end ())
        return i->second.get ();

      if (aps == nullptr)
        return nullptr;
    }

    vector<unique_ptr<package_status_debian>> rs; // Candidates.

    // Translate our package name to the Debian package names.
    //
    {
      auto df = make_diag_frame (
        [&pn] (diag_record& dr)
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
          unique_ptr<package_status_debian> s (
            parse_debian_name (n, need_doc, need_dbg));

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

    // Guess unknown main package given the dev package and its version.
    //
    auto guess_main = [&pn] (package_status_debian& s, const string& ver)
    {
      string depends;
      s.main = main_from_dev (s.dev, ver, depends);

      if (s.main.empty ())
      {
        fail << "unable to guess main Debian package for " << s.dev << ' '
             << ver <<
          info << s.dev << " Depends value: " << depends <<
          info << "consider specifying explicit mapping in " << pn
             << " package manifest";
      }
    };

    // Calculate the package status from individual package components.
    // Return nullopt if there is a component without installed or candidate
    // version (which means the package cannot be installed).
    //
    // @@ Maybe we shouldn't be considering extras for partially_installed
    //    determination?
    //
    using status_type = package_status::status_type;

    auto status = [] (const vector<package_policy>& pps) -> optional<status_type>
    {
      bool i (false), u (false);

      for (const package_policy& pp: pps)
      {
        if (pp.installed_version.empty () && pp.candidate_version.empty ())
          return nullopt;

        (pp.installed_version.empty () ? u : i) = true;
      }

      return (!u ? package_status::installed :
              !i ? package_status::not_installed :
              package_status::partially_installed);
    };

    // First look for an already fully installed package.
    //
    unique_ptr<package_status_debian> r;

    for (unique_ptr<package_status_debian>& ps: rs)
    {
      vector<package_policy>& pps (ps->package_policies);

      if (!ps->main.empty ())            pps.emplace_back (ps->main);
      if (!ps->dev.empty ())             pps.emplace_back (ps->dev);
      if (!ps->doc.empty () && need_doc) pps.emplace_back (ps->doc);
      if (!ps->dbg.empty () && need_dbg) pps.emplace_back (ps->dbg);
      if (!ps->common.empty () && false) pps.emplace_back (ps->common);
      for (const string& n: ps->extras)  pps.emplace_back (n);

      apt_cache_policy (pps);

      // Handle the unknown main package.
      //
      if (ps->main.empty ())
      {
        const package_policy& dev (pps.front ());

        // Note that at this stage we can only use the installed dev package
        // (since the candidate version may change after fetch).
        //
        if (dev.installed_version.empty ())
          continue;

        guess_main (*ps, dev.installed_version);
        pps.emplace (pps.begin (), ps->main);
        apt_cache_policy (pps, 1);
      }

      optional<status_type> s (status (pps));

      if (!s)
        continue;

      if (*s == package_status::installed)
      {
        const package_policy& main (pps.front ());

        ps->status = *s;
        ps->system_name = main.name;
        ps->system_version = main.installed_version;

        if (r != nullptr)
        {
          fail << "multiple installed Debian packages for " << pn <<
            info << "first package: " << r->main << " " << r->system_version <<
            info << "second package: " << ps->main << " " << ps->system_version <<
            info << "consider specifying the desired version manually";
        }

        r = move (ps);
      }
    }

    // Next look for available versions if we are allowed to install.
    //
    if (r == nullptr && install)
    {
      // If we weren't instructed to fetch or we already feteched, then we
      // don't need to re-run apt_cache_policy().
      //
      bool requery;
      if ((requery = fetch && !fetched_))
      {
        // @@ TODO: apt-get update

        fetched_ = true;
      }

      for (unique_ptr<package_status_debian>& ps: rs)
      {
        vector<package_policy>& pps (ps->package_policies);

        if (requery)
          apt_cache_policy (pps);

        // Handle the unknown main package.
        //
        if (ps->main.empty ())
        {
          const package_policy& dev (pps.front ());

          // Note that this time we use the candidate version.
          //
          if (dev.candidate_version.empty ())
          {
            ps = nullptr; // Not installable.
            continue;
          }

          guess_main (*ps, dev.candidate_version);
          pps.emplace (pps.begin (), ps->main);
          apt_cache_policy (pps, 1);

          // @@ What if the main version doesn't match dev? Or it must? Or we
          //    use the candidate_version for main? Fuzzy.
        }

        optional<status_type> s (status (pps));

        if (!s)
        {
          ps = nullptr; // Not installable.
          continue;
        }

        assert (*s != package_status::installed); // Sanity check.

        const package_policy& main (pps.front ());

        ps->status = *s;
        ps->system_name = main.name;
        ps->system_version = main.candidate_version;

        // Prefer partially installed to not installed. This makes detecting
        // ambiguity a bit trickier so we handle partially installed here and
        // not installed in a separate loop below.
        //
        if (ps->status == package_status::partially_installed)
        {
          if (r != nullptr)
          {
            fail << "multiple partially installed Debian packages for " << pn <<
              info << "first package: " << r->main << " " << r->system_version <<
              info << "second package: " << ps->main << " " << ps->system_version <<
              info << "consider specifying the desired version manually";
          }

          r = move (ps);
        }
      }

      if (r == nullptr)
      {
        for (unique_ptr<package_status_debian>& ps: rs)
        {
          if (ps == nullptr)
            continue;

          assert (ps->status != package_status::not_installed); // Sanity check.

          if (r != nullptr)
          {
            fail << "multiple available Debian packages for " << pn <<
              info << "first package: " << r->main << " " << r->system_version <<
              info << "second package: " << ps->main << " " << ps->system_version <<
              info << "consider installing the desired package manually";
          }

          r = move (ps);
        }
      }
    }

    if (r != nullptr)
    {
      // @@ TODO: map system version to bpkg version.
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
