// file      : bpkg/system-package-manager-fedora.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager-fedora.hxx>

#include <bpkg/diagnostics.hxx>

using namespace butl;

namespace bpkg
{
  using package_status = system_package_status_fedora;

  // Parse the fedora-name (or alike) value.
  //
  // Note that for now we treat all the packages from the non-main groups as
  // extras omitting the -common package (assuming it's pulled by the main
  // package) as well as -doc and -dbg unless requested with the
  // extra_{doc,dbg} arguments.
  //
  package_status system_package_manager_fedora::
  parse_name_value (const string& nv,
                    bool extra_doc,
                    bool extra_debuginfo,
                    bool extra_debugsource)
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

      package_status r;

      // Handle the devel instead of main special case for libraries.
      //
      // Check that the following name does not end with -devel. This will be
      // the only way to disambiguate the case where the library name happens
      // to end with -devel (e.g., libops-devel libops-devel-devel).
      //
      {
        string& m (ns[0]);

        if (suffix (m, "-devel") &&
            !(ns.size () > 1 && suffix (ns[1], "-devel")))
        {
          r = package_status ("", move (m));
        }
        else
          r = package_status (move (m));
      }

      // Handle the rest.
      //
      for (size_t i (1); i != ns.size (); ++i)
      {
        string& n (ns[i]);

        const char* w;
        if (string* v = (suffix (n, (w = "-devel"))       ? &r.devel       :
                         suffix (n, (w = "-doc"))         ? &r.doc         :
                         suffix (n, (w = "-debuginfo"))   ? &r.debuginfo   :
                         suffix (n, (w = "-debugsource")) ? &r.debugsource :
                         suffix (n, (w = "-common"))      ? &r.common      :
                                                            nullptr))
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

    package_status r;
    for (size_t i (0); i != gs.size (); ++i)
    {
      if (i == 0) // Main group.
        r = parse_group (gs[i]);
      else
      {
        package_status g (parse_group (gs[i]));

        if (!g.main.empty ())             r.extras.push_back (move (g.main));
        if (!g.devel.empty ())            r.extras.push_back (move (g.devel));
        if (!g.doc.empty () && extra_doc) r.extras.push_back (move (g.doc));

        if (!g.debuginfo.empty () && extra_debuginfo)
          r.extras.push_back (move (g.debuginfo));

        if (!g.debugsource.empty () && extra_debugsource)
          r.extras.push_back (move (g.debugsource));

        if (!g.common.empty () && false)  r.extras.push_back (move (g.common));
        if (!g.extras.empty ())           r.extras.insert (
          r.extras.end (),
          make_move_iterator (g.extras.begin ()),
          make_move_iterator (g.extras.end ()));
      }
    }

    return r;
  }

  static process_path dnf_path;
  static process_path sudo_path;

  // Obtain the installed and candidate versions for the specified list of
  // Fedora packages by executing `dnf list`.
  //
  // If the n argument is not 0, then only query the first n packages.
  //
  void system_package_manager_fedora::
  dnf_list (vector<package_policy>& pps, size_t n)
  {
    if (n == 0)
      n = pps.size ();

    assert (n != 0 && n <= pps.size ());

    // In particular, --quiet makes sure we don't get 'Last metadata
    // expiration check: <timestamp>' printed to stderr. It does not appear to
    // affect error diagnostics (try specifying an unknown package).
    //
    cstrings args {
        "dnf", "list",
        "--all",
        "--cacheonly"
        "--quiet"};

    for (size_t i (0); i != n; ++i)
    {
      package_policy& pp (pps[i]);

      string& n (pp.name);
      assert (!n.empty ());

      pp.installed_version.clear ();
      pp.candidate_version.clear ();

      n += '.';
      n += host_.cpu;

      args.push_back (n.c_str ());
    }

    args.push_back ("dnf.noarch");

    args.push_back (nullptr);

    // Run with the C locale to make sure there is no localization.
    //
    const char* evars[] = {"LC_ALL=C", nullptr};

    try
    {
      if (dnf_path.empty () && !simulate_)
        dnf_path = process::path_search (args[0]);

      process_env pe (dnf_path, evars);

      if (verb >= 3)
        print_process (pe, args);

      // Redirect stdout to a pipe. For good measure also redirect stdin to
      // /dev/null to make sure there are no prompts of any kind.
      //
      process pr;
      if (!simulate_)
        pr = process (dnf_path,
                      args,
                      -2      /* stdin */,
                      -1      /* stdout */,
                      2       /* stderr */,
                      nullptr /* cwd */,
                      evars);
      else
      {
#if 0
        strings k;
        for (size_t i (0); i != n; ++i)
          k.push_back (pps[i].name);

        const path* f (nullptr);
        if (installed_)
        {
          auto i (simulate_->apt_cache_policy_installed_.find (k));
          if (i != simulate_->apt_cache_policy_installed_.end ())
            f = &i->second;
        }
        if (f == nullptr && fetched_)
        {
          auto i (simulate_->apt_cache_policy_fetched_.find (k));
          if (i != simulate_->apt_cache_policy_fetched_.end ())
            f = &i->second;
        }
        if (f == nullptr)
        {
          auto i (simulate_->apt_cache_policy_.find (k));
          if (i != simulate_->apt_cache_policy_.end ())
            f = &i->second;
        }

        diag_record dr (text);
        print_process (dr, pe, args);
        dr << " <" << (f == nullptr || f->empty () ? "/dev/null" : f->string ());

        pr = process (process_exit (0));
        pr.in_ofd = f == nullptr || f->empty ()
          ? fdopen_null ()
          : (f->string () == "-"
             ? fddup (stdin_fd ())
             : fdopen (*f, fdopen_mode::in));
#endif
      }

      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

        // The output of `dnf list <pkg1>.<arch> <pkg2>.<arch> ...` is the 2
        // group of lines in the following form:
        //
        // Installed Packages
        // <pkg1>.<arch>            13.0.0-3.fc35        @<repo1>
        // <pkg2>.<arch>            69.1-6.fc35          @<repo2>
        // Available Packages
        // <pkg1>.<arch>            13.0.1-1.fc35        <repo1>
        // <pkg3>.<arch>            1.2.11-32.fc35       <repo3>
        //
        // Where unknown packages are omitted. The lines order is not
        // necessarily matches the order of the packages on the command line.
        // It looks like there should be not blank lines but who really knows.
        //
        // Note also that if a package appears in the 'Installed Packages'
        // group, then it only appears in the 'Available Packages' if the
        // candidate version is better.
        //
        {
          auto df = make_diag_frame (
            [&pe, &args] (diag_record& dr)
            {
              dr << info << "while parsing output of ";
              print_process (dr, pe, args);
            });

          optional<bool> installed;
          for (string l; !eof (getline (is, l)); )
          {
            if (l == "Installed Packages")
            {
              if (installed)
                fail << "unexpected line '" << l << "': must be first";

              installed = true;
              continue;
            }

            if (l == "Available Packages")
            {
              if (installed && !*installed)
                fail << "duplicate line '" << l << "'";

              installed = false;
              continue;
            }

            if (!installed)
              fail << "unexpected line '" << l << "'";

            size_t e (l.find (' '));

            if (l.empty () || e == 0)
              fail << "expected package name in '" << l << "'";

            if (e == string::npos)
              fail << "expected package version in '" << l << "'";

            string p (l, 0, e);

            size_t b (l.find_first_not_of (' ', e));

            if (b == string::npos)
              fail << "expected package version in '" << l << "'";

            e = l.find (' ', b);

            if (e == string::npos)
              fail << "expected package repository in '" << l << "'";

            string v (l, b, e - b);

            // While we don't really care about the rest of the line, let's
            // for good measure verify that it also contains a repository id.
            //
            b = l.find_first_not_of (' ', e);

            if (b == string::npos)
              fail << "expected package repository in '" << l << "'";

            if (p == "dnf.noarch")
              continue;

            // Find the package.
            //
            auto i (find_if (pps.begin (), pps.end (),
                             [&p] (const package_policy& pp)
                             {return pp.name == p;}));

            if (i == pps.end ())
              fail << "unexpected package name '" << p << "' in '" << l << "'";

            (*installed ? i->installed_version : i->candidate_version) =
              move (v);
          }
        }

        is.close ();
      }
      catch (const io_error& e)
      {
        if (pr.wait ())
          fail << "unable to read " << args[0] << " list output: " << e;

        // Fall through.
      }

      if (!pr.wait ())
      {
        diag_record dr (fail);
        dr << args[0] << " list exited with non-zero code";

        if (verb < 3)
        {
          dr << info << "command line: ";
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

  // Execute `dnf repoquery --requires` and return the dependency packages as
  // a list of the name/version pairs. Fail if either package or version is
  // unknown.
  //
  // Note that if the package or version is unknown then the empty list is
  // returned.
  //
  vector<pair<string, string>> system_package_manager_fedora::
  dnf_repoquery_requires (const string& name, const string& ver)
  {
    assert (!name.empty () && !ver.empty ());

    string spec (name + '-' + ver);

    // In particular, --quiet makes sure we don't get 'Last metadata
    // expiration check: <timestamp>' printed to stderr. It does not appear to
    // affect error diagnostics (try specifying an unknown option).
    //
    const char* args[] = {
      "dnf", "repoquery", "--requires",
      "--resolve",
      "--arch", host_.cpu.c_str (),
      "--qf", "%{name} %{version}-%{release}",
      "--cacheonly",
      "--quiet",
      spec.c_str (),
      nullptr};

    // Note that for this command there seems to be no need to run with the C
    // locale since the output is presumably not localizable. But let's do it
    // for good measure.
    //
    const char* evars[] = {"LC_ALL=C", nullptr};

    vector<pair<string, string>> r;
    try
    {
      if (dnf_path.empty () && !simulate_)
        dnf_path = process::path_search (args[0]);

      process_env pe (dnf_path, evars);

      if (verb >= 3)
        print_process (pe, args);

      // Redirect stdout to a pipe. For good measure also redirect stdin to
      // /dev/null to make sure there are no prompts of any kind.
      //
      process pr;
      if (!simulate_)
        pr = process (dnf_path,
                      args,
                      -2      /* stdin */,
                      -1      /* stdout */,
                      2       /* stderr */,
                      nullptr /* cwd */,
                      evars);
      else
      {
        // @@ TODO
        //
#if 0
        pair<string, string> k (name, ver);

        const path* f (nullptr);
        if (fetched_)
        {
          auto i (simulate_->apt_cache_show_fetched_.find (k));
          if (i != simulate_->apt_cache_show_fetched_.end ())
            f = &i->second;
        }
        if (f == nullptr)
        {
          auto i (simulate_->apt_cache_show_.find (k));
          if (i != simulate_->apt_cache_show_.end ())
            f = &i->second;
        }

        diag_record dr (text);
        print_process (dr, pe, args);
        dr << " <" << (f == nullptr || f->empty () ? "/dev/null" : f->string ());

        if (f == nullptr || f->empty ())
        {
          text << "E: No packages found";
          pr = process (process_exit (100));
        }
        else
        {
          pr = process (process_exit (0));
          pr.in_ofd = f->string () == "-"
            ? fddup (stdin_fd ())
            : fdopen (*f, fdopen_mode::in);
        }
#endif
      }

      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

        // The output of `dnf repoquery --requires <pkg>-<ver>` will be the
        // sequence of the dependency package lines in the `<name> <version>`
        // form. Here is a representative example:
        //
        // bash 5.1.8-3.fc35
        // glibc 2.34-49.fc35
        // libicu 69.1-6.fc35
        // libicu-devel 69.1-6.fc35
        // pkgconf-pkg-config 1.8.0-1.fc35
        //
        for (string l; !eof (getline (is, l)); )
        {
          size_t p (l.find (' '));

          if (p == string::npos)
            fail << "expected package name and version instead of '" << l
                 << "'";

          // Split the line into the package name and version.
          //
          string v (l, p + 1);
          l.resize (p);        // Name.

          // Skip the potential self-dependency line (see the above example).
          //
          if (l == name && v == ver)
            continue;

          r.emplace_back (move (l), move (v));
        }

        is.close ();
      }
      catch (const io_error& e)
      {
        if (pr.wait ())
          fail << "unable to read " << args[0] << " --requires output: " << e;

        // Fall through.
      }

      if (!pr.wait ())
      {
        diag_record dr (fail);
        dr << args[0] << " --requires exited with non-zero code";

        if (verb < 3)
        {
          dr << info << "command line: ";
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

  optional<const system_package_status*> system_package_manager_fedora::
  pkg_status (const package_name& pn, const available_packages* aps)
  {
    // For now we ignore -doc and -debug* package components (but we may want
    // to have options controlling this later). Note also that we assume
    // -common is pulled automatically by the base package so we ignore it as
    // well.
    //
    bool need_doc (false);
    bool need_debuginfo (false);
    bool need_debugsource (false);

    // First check the cache.
    //
    {
      auto i (status_cache_.find (pn));

      if (i != status_cache_.end ())
        return i->second ? &*i->second : nullptr;

      if (aps == nullptr)
        return nullopt;
    }

    vector<package_status> candidates;

    // Translate our package name to the Fedora package names.
    //
    {
      auto df = make_diag_frame (
        [this, &pn] (diag_record& dr)
        {
          dr << info << "while mapping " << pn << " to "
             << os_release_.name_id << " package name";
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
        // @@ We should probably to also/instead consider the project name. We
        //    will need to add it to available_package type then and take it
        //    from the latest available package.
        //
        //    const string* prj (aps != nullptr && aps->front ().project
        //                       ? &aps->front ().project->string ()
        //                       : nullptr);
        //
        if (n.compare (0, 3, "lib") == 0)
        {
          // Keep the base package name empty as an indication that it is to
          // be discovered.
          //
          candidates.push_back (package_status ("", n + "-devel"));

          // @@ Add the project-based candidate.
          //
          // if (prj != nullptr)
          //   candidates.push_back (package_status ("", *prj + "-devel"));
        }
        else
        {
          candidates.push_back (package_status (n));

          // @@ Add the project-based candidate.
          //
          // if (prj != nullptr)
          //   candidates.push_back (package_status ("", *prj));
        }
      }
      else
      {
        // Parse each manual mapping.
        //
        for (const string& n: ns)
        {
          package_status s (
            parse_name_value (n, need_doc, need_debuginfo, need_debugsource));

          // Suppress duplicates for good measure based on the base package
          // name (and falling back to -devel if empty).
          //
          auto i (find_if (candidates.begin (), candidates.end (),
                           [&s] (const package_status& x)
                           {
                             return s.main.empty ()
                               ? s.devel == x.devel
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

    // Guess unknown main package given the devel package and its version.
    //
    auto guess_main = [this, &pn] (package_status& s, const string& ver)
    {
      vector<pair<string, string>> depends (
        dnf_repoquery_requires (s.devel, ver));
#if 0
      s.main = main_from_dev (s.dev, ver, depends);

      if (s.main.empty ())
      {
        fail << "unable to guess main Debian package for " << s.dev << ' '
             << ver <<
          info << s.dev << " Depends value: " << depends <<
          info << "consider specifying explicit mapping in " << pn
             << " package manifest";
      }
#endif
    };

    // First look for an already fully installed package.
    //
    optional<package_status> r;

    for (package_status& ps: candidates)
    {
      vector<package_policy>& pps (ps.package_policies);

      if (!ps.main.empty ())            pps.emplace_back (ps.main);
      if (!ps.devel.empty ())           pps.emplace_back (ps.devel);
      if (!ps.doc.empty () && need_doc) pps.emplace_back (ps.doc);

      if (!ps.debuginfo.empty () && need_debuginfo)
        pps.emplace_back (ps.debuginfo);

      if (!ps.debugsource.empty () && need_debugsource)
        pps.emplace_back (ps.debugsource);

      if (!ps.common.empty () && false) pps.emplace_back (ps.common);
      ps.package_policies_main = pps.size ();
      for (const string& n: ps.extras)  pps.emplace_back (n);

      dnf_list (pps);

      // Handle the unknown main package.
      //
      if (ps.main.empty ())
      {
        const package_policy& devel (pps.front ());

        // Note that at this stage we can only use the installed devel package
        // (since the candidate version may change after fetch).
        //
        if (devel.installed_version.empty ())
          continue;

        guess_main (ps, devel.installed_version);
        pps.emplace (pps.begin (), ps.main);
        ps.package_policies_main++;
        dnf_list (pps, 1);
      }

#if 0
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
          fail << "multiple installed " << os_release_.name_id
               << " packages for " << pn <<
            info << "first package: " << r->main << " " << r->system_version <<
            info << "second package: " << ps.main << " " << ps.system_version <<
            info << "consider specifying the desired version manually";
        }

        r = move (ps);
      }
#endif
    }

    // Cache.
    //
    auto i (status_cache_.emplace (pn, move (r)).first);
    return i->second ? &*i->second : nullptr;
  }

  void system_package_manager_fedora::
  pkg_install (const vector<package_name>& /*pns*/)
  {
    // @@ TODO
  }
}
