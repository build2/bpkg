// file      : bpkg/system-package-manager-debian.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager-debian.hxx>

#include <bpkg/diagnostics.hxx>

namespace bpkg
{
  using package_status = system_package_status;
  using package_status_debian = system_package_status_debian;

  using package_policy = package_status_debian::package_policy;

  // Parse the debian-name (or alike) value.
  //
  // The format of this value is a comma-separated list of one or more package
  // groups:
  //
  // <package-group> [, <package-group>...]
  //
  // Where each <package-group> is the space-separated list of one or more
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
  // We allow/recommend specifying the -dev package instead of the main
  // package for libraries (the name starts with lib), seeing that we are
  // capable of detecting the main package automatically. If the library name
  // happens to end with -dev (which poses an ambiguity), then the -dev
  // package should be specified explicitly as the second package to
  // disambiguate this situation (if a non-library name happened to start with
  // lib and end with -dev, well, you are out of luck, I guess).
  //
  // Note also that for now we treat all the packages from the non-main groups
  // as extras (but in the future we may decide to sort them out like the main
  // group). For now we omit the -common package (assuming it's pulled by the
  // main package) as well as -doc and -dbg unless requested (the
  // extra_{doc,dbg} arguments).
  //
  static package_status_debian
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

      package_status_debian r;

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
          r = package_status_debian ("", move (m));
        }
        else
          r = package_status_debian (move (m));
      }

      // Handle the rest.
      //
      for (size_t i (1); i != ns.size (); ++i)
      {
        string& n (ns[i]);

        const char* w;
        if (string* v = (suffix (n, (w = "-dev"))    ? &r.dev :
                         suffix (n, (w = "-doc"))    ? &r.doc :
                         suffix (n, (w = "-dbg"))    ? &r.dbg :
                         suffix (n, (w = "-common")) ? &r.common : nullptr))
        {
          if (!v->empty ())
            fail << "multiple " << w << " package names in '" << g << "'" <<
              info << "did you forget to separate package groups with comma?";

          *v = move (n);
        }
        else
          r.extras.push_back (move (n));
      }

      return r;
    };

    strings gs (split (nv, ','));
    assert (!gs.empty ()); // *-name value cannot be empty.

    package_status_debian r;
    for (size_t i (0); i != gs.size (); ++i)
    {
      if (i == 0) // Main group.
        r = parse_group (gs[i]);
      else
      {
        package_status_debian g (parse_group (gs[i]));

        if (!g.main.empty ())             r.extras.push_back (move (g.main));
        if (!g.dev.empty ())              r.extras.push_back (move (g.dev));
        if (!g.doc.empty () && extra_doc) r.extras.push_back (move (g.doc));
        if (!g.dbg.empty () && extra_dbg) r.extras.push_back (move (g.dbg));
        if (!g.common.empty () && false)  r.extras.push_back (move (g.common));
        if (!g.extras.empty ())           r.extras.insert (
          r.extras.end (),
          make_move_iterator (g.extras.begin ()),
          make_move_iterator (g.extras.end ()));
      }
    }

    return r;
  }

  // Do we use apt or apt-get? From apt(8):
  //
  // "The apt(8) commandline is designed as an end-user tool and it may change
  //  behavior between versions. [...]
  //
  //  All features of apt(8) are available in dedicated APT tools like
  //  apt-get(8) and apt-cache(8) as well. [...] So you should prefer using
  //  these commands (potentially with some additional options enabled) in
  //  your scripts as they keep backward compatibility as much as possible."
  //
  static process_path apt_cache_path;
  static process_path apt_get_path;
  static process_path sudo_path;

  // Obtain the installed and candidate versions for the specified list
  // of Debian packages by executing `apt-cache policy`.
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
      if (apt_cache_path.empty ())
        apt_cache_path = process::path_search (args[0]);

      process_env pe (apt_cache_path, evars);

      if (verb >= 3)
        print_process (pe, args);

      // Redirect stdout to a pipe. For good measure also redirect stdin to
      // /dev/null to make sure there are no prompts of any kind.
      //
      process pr (apt_cache_path,
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
        // Note also that if Installed version is not (none), then the
        // Candidate version will be that version of better.
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

  // Execute `apt-cache show` and return the Depends value, if any, for the
  // specified package and version.  Fail if either package or version is
  // unknown.
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
      if (apt_cache_path.empty ())
        apt_cache_path = process::path_search (args[0]);

      process_env pe (apt_cache_path, evars);

      if (verb >= 3)
        print_process (pe, args);

      // Redirect stdout to a pipe. For good measure also redirect stdin to
      // /dev/null to make sure there are no prompts of any kind.
      //
      process pr (apt_cache_path,
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

  // Prepare the common `apt-get <command>` options.
  //
  static pair<cstrings, const process_path&>
  apt_get_common (const char* command,
                  const string& sudo,
                  optional<bool> progress,
                  bool yes)
  {
    cstrings args;

    if (!sudo.empty ())
      args.push_back (sudo.c_str ());

    args.push_back ("apt-get");
    args.push_back (command);

    // Map our verbosity/progress to apt-get --quiet[=<level>]. The levels
    // appear to have the following behavior:
    //
    // 1 -- shows URL being downloaded but no percentage progress is shown.
    //
    // 2 -- only shows diagnostics (implies --assume-yes and which cannot be
    //      overriden with --assume-no).
    //
    // It also appears to automatically use level 1 if stderr is not a
    // terminal. This can be overrident with --quiet=0.
    //
    // Note also that --show-progress does not apply to apt-get update. For
    // apt-get install it shows additionally progress during unpacking which
    // looks quite odd.
    //
    if (progress && *progress)
    {
      args.push_back ("--quiet=0");
    }
    else if (verb == 0)
    {
      // Only use level 2 if assuming yes.
      //
      args.push_back (yes ? "--quiet=2" : "--quiet");
    }
    else if (progress && !*progress)
    {
      args.push_back ("--quiet");
    }

    if (yes)
    {
      args.push_back ("--assume-yes");
    }
    else if (!stderr_term)
    {
      // Suppress any prompts if stderr is not a terminal for good measure.
      //
      args.push_back ("--assume-no");
    }

    try
    {
      const process_path* pp (nullptr);

      if (!sudo.empty ())
      {
        if (sudo_path.empty ())
          sudo_path = process::path_search (args[0]);

        pp = &sudo_path;
      }
      else
      {
        if (apt_get_path.empty ())
          apt_get_path = process::path_search (args[0]);

        pp = &apt_get_path;
      }

      return pair<cstrings, const process_path&> (move (args), *pp);
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  // Execute `apt-get update` to update the package index.
  //
  static void
  apt_get_update (const string& sudo, optional<bool> progress, bool yes)
  {
    pair<cstrings, const process_path&> args_pp (
      apt_get_common ("update", sudo, progress, yes));

    cstrings& args (args_pp.first);
    const process_path& pp (args_pp.second);

    args.push_back (nullptr);

    try
    {
      if (verb >= 2)
        print_process (args);
      else if (verb)
        text << "updating Debian package index...";

      // We don't expect any prompts from apt-get update, but who knows.
      //
      process pr (pp, args);

      if (!pr.wait ())
      {
        diag_record dr (fail);
        dr << "apt-get update exited with non-zero code";

        if (verb < 3)
        {
          dr << "command line: ";
          print_process (dr, args);
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

  // Execute `apt-get install` to install the specified packages/version
  // (e.g., libfoo or libfoo=1.2.3).
  //
  static void
  apt_get_install (const strings& pkgs,
                   const string& sudo,
                   optional<bool> progress,
                   bool yes)
  {
    assert (!pkgs.empty ());

    pair<cstrings, const process_path&> args_pp (
      apt_get_common ("install", sudo, progress, yes));

    cstrings& args (args_pp.first);
    const process_path& pp (args_pp.second);

    for (const string& p: pkgs)
      args.push_back (p.c_str ());

    args.push_back (nullptr);

    try
    {
      if (verb >= 2)
        print_process (args);
      else if (verb)
        text << "installing Debian packages...";

      process pr (pp, args);

      if (!pr.wait ())
      {
        diag_record dr (fail);
        dr << "apt-get install exited with non-zero code";

        if (verb < 3)
        {
          dr << "command line: ";
          print_process (dr, args);
        }

        dr << "consider resolving the issue manually and retrying the "
           << "bpkg command";
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
        return i->second ? &*i->second : nullptr;

      if (aps == nullptr)
        return nullopt;
    }

    vector<package_status_debian> candidates;

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
        if (n.compare (0, 3, "lib") == 0)
        {
          // Keep the main package name empty as an indication that it is to
          // be discovered.
          //
          candidates.push_back (package_status_debian ("", n + "-dev"));
        }
        else
          candidates.push_back (package_status_debian (n));
      }
      else
      {
        // Parse each manual mapping.
        //
        for (const string& n: ns)
        {
          package_status_debian s (parse_debian_name (n, need_doc, need_dbg));

          // Suppress duplicates for good measure based on the main package
          // name (and falling back to -dev if empty).
          //
          auto i (find_if (candidates.begin (), candidates.end (),
                           [&s] (const package_status_debian& x)
                           {
                             return s.main.empty ()
                               ? s.dev == x.dev
                               : s.main == x.main;
                           }));
          if (i == candidates.end ())
            candidates.push_back (move (s));
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
    // The main argument specifies the size of the main group. Only components
    // from this group are considered for partially_installed determination.
    //
    using status_type = package_status::status_type;

    auto status = [] (const vector<package_policy>& pps, size_t main)
      -> optional<status_type>
    {
      bool i (false), u (false);

      for (size_t j (0); j != pps.size (); ++j)
      {
        const package_policy& pp (pps[j]);

        if (pp.installed_version.empty ())
        {
          if (pp.candidate_version.empty ())
            return nullopt;

          u = true;
        }
        else if (j < main)
          i = true;
      }

      return (!u ? package_status::installed :
              !i ? package_status::not_installed :
              package_status::partially_installed);
    };

    // First look for an already fully installed package.
    //
    optional<package_status_debian> r;

    for (package_status_debian& ps: candidates)
    {
      vector<package_policy>& pps (ps.package_policies);

      if (!ps.main.empty ())            pps.emplace_back (ps.main);
      if (!ps.dev.empty ())             pps.emplace_back (ps.dev);
      if (!ps.doc.empty () && need_doc) pps.emplace_back (ps.doc);
      if (!ps.dbg.empty () && need_dbg) pps.emplace_back (ps.dbg);
      if (!ps.common.empty () && false) pps.emplace_back (ps.common);
      ps.package_policies_main = pps.size ();
      for (const string& n: ps.extras)  pps.emplace_back (n);

      apt_cache_policy (pps);

      // Handle the unknown main package.
      //
      if (ps.main.empty ())
      {
        const package_policy& dev (pps.front ());

        // Note that at this stage we can only use the installed dev package
        // (since the candidate version may change after fetch).
        //
        if (dev.installed_version.empty ())
          continue;

        guess_main (ps, dev.installed_version);
        pps.emplace (pps.begin (), ps.main);
        ps.package_policies_main++;
        apt_cache_policy (pps, 1);
      }

      optional<status_type> s (status (pps, ps.package_policies_main));

      if (!s)
        continue;

      if (*s == package_status::installed)
      {
        const package_policy& main (pps.front ());

        ps.status = *s;
        ps.system_name = main.name;
        ps.system_version = main.installed_version;

        if (r)
        {
          fail << "multiple installed Debian packages for " << pn <<
            info << "first package: " << r->main << " " << r->system_version <<
            info << "second package: " << ps.main << " " << ps.system_version <<
            info << "consider specifying the desired version manually";
        }

        r = move (ps);
      }
    }

    // Next look for available versions if we are allowed to install.
    //
    if (!r && install)
    {
      // If we weren't instructed to fetch or we already fetched, then we
      // don't need to re-run apt_cache_policy().
      //
      bool requery;
      if ((requery = fetch && !fetched_))
      {
        apt_get_update ("sudo" /* --sys-sudo */, progress_, false /* --sys-yes */);
        fetched_ = true;
      }

      for (package_status_debian& ps: candidates)
      {
        vector<package_policy>& pps (ps.package_policies);

        if (requery)
          apt_cache_policy (pps);

        // Handle the unknown main package.
        //
        if (ps.main.empty ())
        {
          const package_policy& dev (pps.front ());

          // Note that this time we use the candidate version.
          //
          if (dev.candidate_version.empty ())
            continue; // Not installable.

          guess_main (ps, dev.candidate_version);
          pps.emplace (pps.begin (), ps.main);
          ps.package_policies_main++;
          apt_cache_policy (pps, 1);
        }

        optional<status_type> s (status (pps, ps.package_policies_main));

        if (!s)
        {
          ps.main.clear (); // Not installable.
          continue;
        }

        assert (*s != package_status::installed); // Sanity check.

        const package_policy& main (pps.front ());

        // Note that if we are installing something for this main package,
        // then we always go for the candidate version even though it may have
        // an installed version that may be good enough (especially if what we
        // are installing are extras). The reason is that it may as well not
        // be good enough (especially if we are installing the -dev package)
        // and there is no straightforward way to change our mind.
        //
        ps.status = *s;
        ps.system_name = main.name;
        ps.system_version = main.candidate_version;

        // Prefer partially installed to not installed. This makes detecting
        // ambiguity a bit trickier so we handle partially installed here and
        // not installed in a separate loop below.
        //
        if (ps.status == package_status::partially_installed)
        {
          if (r)
          {
            fail << "multiple partially installed Debian packages for " << pn <<
              info << "first package: " << r->main << " " << r->system_version <<
              info << "second package: " << ps.main << " " << ps.system_version <<
              info << "consider specifying the desired version manually";
          }

          r = move (ps);
        }
      }

      if (!r)
      {
        for (package_status_debian& ps: candidates)
        {
          if (ps.main.empty ())
            continue;

          assert (ps.status != package_status::not_installed); // Sanity check.

          if (r)
          {
            fail << "multiple available Debian packages for " << pn <<
              info << "first package: " << r->main << " " << r->system_version <<
              info << "second package: " << ps.main << " " << ps.system_version <<
              info << "consider installing the desired package manually";
          }

          r = move (ps);
        }
      }
    }

    if (r)
    {
      // Map the system version to the bpkg version.
      //
      optional<version> v (
        downstream_package_version (r->system_version,
                                    *aps,
                                    os_release_.name_id,
                                    os_release_.version_id,
                                    os_release_.like_ids));

      if (!v)
      {
        // Fallback to using system version as downstream version.
        //
        try
        {
          v = version (r->system_version);
        }
        catch (const invalid_argument& e)
        {
          fail << "unable to map Debian package " << r->system_name
               << " version " << r->system_version << " to bpkg package "
               << pn << " version" <<
            info << "Debian version is not a valid bpkg version: " << e.what () <<
            info << "consider specifying explicit mapping in " << pn
               << " package manifest";
        }
      }

      r->version = move (*v);
    }

    // Cache.
    //
    auto i (status_cache_.emplace (pn, move (r)).first);
    return i->second ? &*i->second : nullptr;
  }

  void system_package_manager_debian::
  pkg_install (const vector<package_name>& pns, bool /* @@ install */)
  {
    assert (!pns.empty ());

    assert (!installed_);
    installed_ = true;

    // Collect and merge all the Debian packages/version for the specified
    // bpkg packages.
    //
    struct package
    {
      string name;
      string version; // Empty if unspecified.
    };
    vector<package> pkgs;

    for (const package_name& pn: pns)
    {
      auto it (status_cache_.find (pn));
      assert (it != status_cache_.end () && it->second);

      const package_status_debian& ps (*it->second);

      // At first it may seem we don't need to do anything for already fully
      // installed packages. But it's possible some of them were automatically
      // installed, meaning that they can be automatically removed if they no
      // longer have any dependents (see apt-mark(8) for details). Which in
      // turn means that things may behave differently depending on whether
      // we've installed a package ourselves or if it was already installed.
      // So instead we are going to also pass the already fully installed
      // packages which will make sure they are all set to manually installed.
      // But we must be careful not to force their upgrade. To achieve this
      // we will specify the installed version as the desired version.
      //
      // Note also that for partially/not installed we don't specify the
      // version, expecting the candidate version to be installed.
      //
      bool fi (ps.status == package_status::installed);

      for (const package_policy& pp: ps.package_policies)
      {
        string n (pp.name);
        string v (fi ? pp.installed_version : string ());

        auto i (find_if (pkgs.begin (), pkgs.end (),
                         [&n] (const package& p)
                         {
                           return p.name == n;
                         }));

        if (i != pkgs.end ())
        {
          if (i->version.empty ())
            i->version = move (v);
          else
            // Feels like this cannot happen since we always use the installed
            // version of the package.
            //
            assert (i->version == v);
        }
        else
          pkgs.push_back (package {move (n), move (v)});
      }
    }

    // Install.
    //
    {
      // Convert to the `apt-get install` <pkg>[=<ver>] form.
      //
      strings specs;
      specs.reserve (pkgs.size ());
      for (const package& p: pkgs)
      {
        string s (p.name);
        if (!p.version.empty ())
        {
          s += '=';
          s += p.version;
        }
        specs.push_back (move (s));
      }

      apt_get_install (specs,
                       "sudo" /* --sys-sudo */,
                       progress_,
                       false /* --sys-yes */);
    }

    // Verify that versions we have promised in pkg_status() match what
    // actually got installed.
    //
    {
      vector<package_policy> pps;

      for (const package_name& pn: pns)
      {
        const package_status_debian& ps (*status_cache_.find (pn)->second);
        pps.push_back (package_policy (ps.system_name));
      }

      apt_cache_policy (pps);

      auto i (pps.begin ());
      for (const package_name& pn: pns)
      {
        const package_status_debian& ps (*status_cache_.find (pn)->second);
        const package_policy& pp (*i++);

        if (pp.installed_version != ps.system_version)
        {
          fail << "unexpected Debian package version for " << ps.system_name <<
            info << "expected: " << ps.system_version <<
            info << "installed: " << pp.installed_version <<
            info << "consider retrying the bpkg command";
        }
      }
    }
  }
}
