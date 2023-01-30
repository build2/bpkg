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
  parse_name_value (const package_name& pn,
                    const string& nv,
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

    auto parse_group = [&split, &suffix] (const string& g,
                                          const package_name* pn)
    {
      strings ns (split (g, ' '));

      if (ns.empty ())
        fail << "empty package group";

      package_status r;

      // Handle the "devel instead of main" special case for libraries.
      //
      // Note: the lib prefix check is based on the bpkg package name.
       //
      // Check that the following name does not end with -devel. This will be
      // the only way to disambiguate the case where the library name happens
      // to end with -devel (e.g., libops-devel libops-devel-devel).
      //
      {
        string& m (ns[0]);

        if (pn != nullptr                            &&
            pn->string ().compare (0, 3, "lib") == 0 &&
            suffix (m, "-devel")                     &&
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
        r = parse_group (gs[i], &pn);
      else
      {
        package_status g (parse_group (gs[i], nullptr));

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

  // Attempt to determine the main package name from its -devel package based
  // on the extracted dependencies. Return empty string if unable to.
  //
  string system_package_manager_fedora::
  main_from_dev (const string& devel_name,
                 const string& devel_ver,
                 const vector<pair<string, string>>& depends)
  {
    // For the main package we look for a dependency with the name
    // <devel-stem> and the devel_ver version. Failed that, try the
    // <devel-stem>-libs name instead.
    //
    string devel_stem (devel_name, 0, devel_name.rfind ("-devel"));

    auto find = [&devel_ver, &depends] (const string& n)
    {
      auto i (find_if (depends.begin (), depends.end (),
                       [&n, &devel_ver] (const pair<string, string>& d)
                       {
                         return d.first == n && d.second == devel_ver;
                       }));

      return i != depends.end () ? i->first : empty_string;
    };

    // Note that for a mixed package we need to rather end up with the -libs
    // subpackage rather that with the base package. Think of the following
    // package:
    //
    // openssl openssl-libs openssl-devel
    //
    string r (find (devel_stem + "-libs"));
    return !r.empty () ? r : find (devel_stem);
  }

  static process_path dnf_path;
  static process_path sudo_path;

  // Obtain the installed and candidate versions for the specified list of
  // Fedora packages by executing `dnf list`.
  //
  // If the n argument is not 0, then only query the first n packages.
  //
  void system_package_manager_fedora::
  dnf_list (vector<package_info>& pis, size_t n)
  {
    if (n == 0)
      n = pis.size ();

    assert (n != 0 && n <= pis.size ());

    // The --quiet option makes sure we don't get 'Last metadata expiration
    // check: <timestamp>' printed to stderr. It does not appear to affect
    // error diagnostics (try specifying an unknown package).
    //
    cstrings args {
        "dnf", "list",
        "--all",
        "--cacheonly",
        "--quiet"};

    for (size_t i (0); i != n; ++i)
    {
      package_info& pi (pis[i]);

      string& n (pi.name);
      assert (!n.empty ());

      pi.installed_version.clear ();
      pi.candidate_version.clear ();

      pi.installed_arch.clear ();
      pi.candidate_arch.clear ();

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

          // If true, then we inside the 'Installed Packages' section. If
          // false, then we inside the 'Available Packages' section. Initially
          // nullopt.
          //
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
            // verify that it also contains a repository id, for good measure.
            //
            b = l.find_first_not_of (' ', e);

            if (b == string::npos)
              fail << "expected package repository in '" << l << "'";

            // Skip the special dnf package.
            //
            if (p == "dnf.noarch")
              continue;

            // Separate the architecture from the package name.
            //
            e = p.rfind ('.');

            if (e == string::npos || e == p.size () - 1)
              fail << "can't deduce architecture for package '" << p
                   << "' in '" << l << "'";

            string a (p, e + 1);

            // Skip the package of a different architecture.
            //
            if (a != host_.cpu && a != "noarch")
              continue;

            p.resize (e);

            // Find the package info to update.
            //
            auto i (find_if (pis.begin (), pis.end (),
                             [&p] (const package_info& pi)
                             {return pi.name == p;}));

            if (i == pis.end ())
              fail << "unexpected package name '" << p << "' in '" << l << "'";

            string& ver (*installed
                         ? i->installed_version
                         : i->candidate_version);

            if (!ver.empty ())
              fail << "multiple " << (*installed ? "installed " : "available ")
                   << "versions of package '" << p << "'" <<
                info << "first:  " << ver <<
                info << "second: " << v;

            ver = move (v);

            (*installed ? i->installed_arch : i->candidate_arch) = move (a);
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
  dnf_repoquery_requires (const string& name,
                          const string& ver,
                          const string& arch)
  {
    assert (!name.empty () && !ver.empty ());

    // Qualify the package with an architecture suffix.
    //
    // Note that by reason unknown, the below command may also print
    // dependency packages of different architectures. It feels sensible to
    // just skip them.
    //
    string spec (name + '-' + ver + '.' + arch);

    // In particular, --quiet makes sure we don't get 'Last metadata
    // expiration check: <timestamp>' printed to stderr. It does not appear to
    // affect error diagnostics (try specifying an unknown option).
    //
    const char* args[] = {
      "dnf", "repoquery", "--requires",
      "--resolve",
      "--qf", "%{name} %{arch} %{epoch}:%{version}-%{release}",
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

        // The output of the command will be the sequence of the dependency
        // package lines in the `<name> <arc> <version>` form. So for example
        // for the libicu-devel-69.1-6.fc35.x86_64 package it is as follows:
        //
        // bash i686 0:5.1.8-3.fc35
        // bash x86_64 0:5.1.8-3.fc35
        // glibc i686 0:2.34-49.fc35
        // glibc x86_64 0:2.34-49.fc35
        // libicu x86_64 0:69.1-6.fc35
        // libicu-devel i686 0:69.1-6.fc35
        // libicu-devel x86_64 0:69.1-6.fc35
        // pkgconf-pkg-config i686 0:1.8.0-1.fc35
        // pkgconf-pkg-config x86_64 0:1.8.0-1.fc35
        //
        for (string l; !eof (getline (is, l)); )
        {
          size_t e (l.find (' '));

          if (l.empty () || e == 0)
            fail << "expected package name in '" << l << "'";

          if (e == string::npos)
            fail << "expected package architecture in '" << l << "'";

          string p (l, 0, e);

          size_t b (e + 1);
          e = l.find (' ', b);

          if (e == string::npos)
            fail << "expected package version in '" << l << "'";

          string a (l, b, e - b);
          if (a.empty ())
            fail << "expected package architecture in '" << l << "'";

          string v (l, e + 1);

          // Strip the '0:' epoch to align with package versions retrieved by
          // other functions (dnf_list(), etc).
          //
          e = v.find (':');
          if (e == string::npos || e == 0)
            fail << "no epoch for package version in '" << l << "'";

          if (e == 1 && v[0] == '0')
            v.erase (0, 2);

          // Skip the potential self-dependency line (see the above example)
          // and dependencies of a different architecture.
          //
          if (l == name || (a != host_.cpu && a != "noarch"))
            continue;

          r.emplace_back (move (p), move (v));
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

  // Prepare the common options for `dnf makecache` and `dnf install`
  // commands.
  //
  pair<cstrings, const process_path&> system_package_manager_fedora::
  dnf_common (const char* command)
  {
    cstrings args;

    if (!sudo_.empty ())
      args.push_back (sudo_.c_str ());

    args.push_back ("dnf");
    args.push_back (command);

    // Map our verbosity/progress to dnf --quiet and --verbose options.
    //
    // Note that all the diagnostics, including the progress indication but
    // excluding error messages, is printed to stdout. By default the progress
    // bar for network transfers is printed, unless stdout is not a terminal.
    // The --quiet option disables printing the plan and all the progress
    // output, but not the confirmation prompt nor error messages.
    //
    if (progress_ && *progress_)
    {
      // Print the progress bar by default, unless this is not a terminal.
    }
    else if (verb == 0)
    {
      args.push_back ("--quiet");
    }
    else if (verb > 3)
    {
      args.push_back ("--verbose");
    }
    else if (progress_ && !*progress_)
    {
      args.push_back ("--quiet");
    }

    if (yes_)
    {
      args.push_back ("--assumeyes");
    }
    else if (!stderr_term)
    {
      // Suppress any prompts if stderr is not a terminal for good measure.
      //
      args.push_back ("--assumeno");
    }

    try
    {
      const process_path* pp (nullptr);

      if (!sudo_.empty ())
      {
        if (sudo_path.empty () && !simulate_)
          sudo_path = process::path_search (args[0]);

        pp = &sudo_path;
      }
      else
      {
        if (dnf_path.empty () && !simulate_)
          dnf_path = process::path_search (args[0]);

        pp = &dnf_path;
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

  // Execute `dnf makecache` to download and cache the repositories metadata.
  //
  void system_package_manager_fedora::
  dnf_makecache ()
  {
    pair<cstrings, const process_path&> args_pp (dnf_common ("makecache"));

    cstrings& args (args_pp.first);
    const process_path& pp (args_pp.second);

    args.push_back ("--refresh");
    args.push_back (nullptr);

    try
    {
      if (verb >= 2)
        print_process (args);
      else if (verb == 1)
        text << "updating " << os_release_.name_id
             << " repositories metadata...";

      process pr;
      if (!simulate_)
        pr = process (pp, args, 0 /* stdin */, 2 /* stdout */);
      else
      {
        print_process (args);
        // @@ TODO
        //pr = process (process_exit (simulate_->apt_get_update_fail_ ? 100 : 0));
      }

      if (!pr.wait ())
      {
        diag_record dr (fail);
        dr << "dnf makecache exited with non-zero code";

        if (verb < 2)
        {
          dr << info << "command line: ";
          print_process (dr, args);
        }
      }

      if (verb == 1)
        text << "updated " << os_release_.name_id << " repositories metadata";
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  // Execute `dnf install` to install the specified packages/versions (e.g.,
  // libfoo or libfoo-1.2.3) and the `dnf mark install` to mark the specified
  // packages as installed by user.
  //
  void system_package_manager_fedora::
  dnf_install (const strings& pkgs)
  {
    assert (!pkgs.empty ());

    // Install.
    //
    {
      pair<cstrings, const process_path&> args_pp (dnf_common ("install"));

      cstrings& args (args_pp.first);
      const process_path& pp (args_pp.second);

      // Note that we can't use --cacheonly here to prevent the metadata
      // update, since the install command expects the package RPM files to
      // also be cached then and fails if that's not the case. Thus we
      // override the metadata_expire=never configuration option instead.
      //
      args.push_back ("--setopt=metadata_expire=never");

      for (const string& p: pkgs)
        args.push_back (p.c_str ());

      args.push_back (nullptr);

      try
      {
        if (verb >= 2)
          print_process (args);
        else if (verb == 1)
          text << "installing " << os_release_.name_id << " packages...";

        process pr;
        if (!simulate_)
          pr = process (pp, args, 0 /* stdin */, 2 /* stdout */);
        else
        {
          print_process (args);
          // @@ TODO
          //pr = process (process_exit (simulate_->apt_get_install_fail_ ? 100 : 0));
        }

        if (!pr.wait ())
        {
          diag_record dr (fail);
          dr << "dnf install exited with non-zero code";

          if (verb < 2)
          {
            dr << info << "command line: ";
            print_process (dr, args);
          }

          dr << info << "consider resolving the issue manually and retrying "
             << "the bpkg command";
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

    // Mark as installed.
    //
    {
      pair<cstrings, const process_path&> args_pp (dnf_common ("mark"));

      cstrings& args (args_pp.first);
      const process_path& pp (args_pp.second);

      args.push_back ("install");
      args.push_back ("--cacheonly");

      for (const string& p: pkgs)
        args.push_back (p.c_str ());

      args.push_back (nullptr);

      try
      {
        if (verb >= 2)
          print_process (args);

        process pr;
        if (!simulate_)
          pr = process (pp, args, 0 /* stdin */, 2 /* stdout */);
        else
        {
          print_process (args);
          // @@ TODO
          //pr = process (process_exit (simulate_->apt_get_install_fail_ ? 100 : 0));
        }

        if (!pr.wait ())
        {
          diag_record dr (fail);
          dr << "dnf mark install exited with non-zero code";

          if (verb < 2)
          {
            dr << info << "command line: ";
            print_process (dr, args);
          }

          dr << info << "consider resolving the issue manually and retrying "
             << "the bpkg command";
        }

        if (verb == 1)
          text << "installed " << os_release_.name_id << " packages";
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e;

        if (e.child)
          exit (1);

        throw failed ();
      }
    }
  }

  optional<const system_package_status*> system_package_manager_fedora::
  pkg_status (const package_name& pn, const available_packages* aps)
  {
    // For now we ignore -doc and -debug* package components (but we may want
    // to have options controlling this later). Note also that we assume
    // -common is pulled automatically by the base package so we ignore it as
    // well (see equivalent logic in parse_name_value()).
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
        // details). Failed that we should try to use the project name, if
        // present, instead.
        //
        const string& n (pn.string ());

        assert (!aps->empty ());

        const shared_ptr<available_package>& ap (aps->front ().first);
        string p (ap->project && *ap->project != n
                  ? ap->project->string ()
                  : empty_string);

        // The best we can do in trying to detect whether this is a library is
        // to check for the lib prefix. Libraries without the lib prefix and
        // non-libraries with the lib prefix (both of which we do not
        // recomment) will have to provide a manual mapping.
        //
        if (n.compare (0, 3, "lib") == 0)
        {
          if (!p.empty ())
            p += "-devel";

          // Keep the base package name empty as an indication that it is to
          // be discovered.
          //
          candidates.push_back (package_status ("", n + "-devel", move (p)));
        }
        else
          candidates.push_back (package_status (n, "", move (p)));
      }
      else
      {
        // Parse each manual mapping.
        //
        for (const string& n: ns)
        {
          package_status s (parse_name_value (pn,
                                              n,
                                              need_doc,
                                              need_debuginfo,
                                              need_debugsource));

          // Suppress duplicates for good measure based on the base package
          // name (and falling back to -devel if empty).
          //
          auto i (find_if (candidates.begin (), candidates.end (),
                           [&s] (const package_status& x)
                           {
                             // Note that it's possible for one mapping to be
                             // specified as -devel only while the other as
                             // main and -devel.
                             //
                             return s.main.empty () || x.main.empty ()
                                    ? s.devel == x.devel
                                    : s.main == x.main;
                           }));
          if (i == candidates.end ())
            candidates.push_back (move (s));
          else
          {
             // Should we verify the rest matches for good measure? But what if
             // we need to override, as in:
             //
             // fedora_35-name: libfoo libfoo-bar-dev
             // fedora_34-name: libfoo libfoo-dev
             //
             // Note that for this to work we must get fedora_35 values before
             // fedora_34, which is the semantics guaranteed by
             // system_package_names().
          }
        }
      }
    }

    // Guess unknown main package given the -devel package and its version.
    //
    auto guess_main = [this, &pn] (package_status& s,
                                   const string& ver,
                                   const string& arch)
    {
      vector<pair<string, string>> depends (
        dnf_repoquery_requires (s.devel, ver, arch));

      s.main = main_from_dev (s.devel, ver, depends);

      if (s.main.empty ())
      {
        diag_record dr (fail);
        dr << "unable to guess main " << os_release_.name_id
             << " package for " << s.devel << ' ' << ver <<
          info << "depends on";

        bool first (true);
        for (const pair<string, string>& d: depends)
        {
          if (first)
            first = false;
          else
            dr << ',';

          dr << d.first << ' ' << d.second;
        }

        dr << info << "consider specifying explicit mapping in " << pn
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
    // @@ TODO: we should probably prioritize partially installed with fully
    // installed main group. Add almost_installed next to partially_installed?
    //
    using status_type = package_status::status_type;

    auto status = [] (const vector<package_info>& pis, size_t main)
      -> optional<status_type>
    {
      bool i (false), u (false);

      for (size_t j (0); j != pis.size (); ++j)
      {
        const package_info& pi (pis[j]);

        if (pi.installed_version.empty ())
        {
          if (pi.candidate_version.empty ())
            return nullopt;

          u = true;
        }
        else if (j < main)
          i = true;
      }

      return (!u ? package_status::installed     :
              !i ? package_status::not_installed :
              package_status::partially_installed);
    };

    // First look for an already fully installed package.
    //
    // But first, choose between the package name-based and project-based
    // guessed system package name.
    //
    for (package_status& ps: candidates)
    {
      vector<package_info>& pis (ps.package_infos);

      if (!ps.main.empty ())            pis.emplace_back (ps.main);
      if (!ps.devel.empty ())           pis.emplace_back (ps.devel);
      if (!ps.fallback.empty ())        pis.emplace_back (ps.fallback);
      if (!ps.doc.empty () && need_doc) pis.emplace_back (ps.doc);

      if (!ps.debuginfo.empty () && need_debuginfo)
        pis.emplace_back (ps.debuginfo);

      if (!ps.debugsource.empty () && need_debugsource)
        pis.emplace_back (ps.debugsource);

      if (!ps.common.empty () && false) pis.emplace_back (ps.common);
      ps.package_infos_main = pis.size ();
      for (const string& n: ps.extras)  pis.emplace_back (n);

      dnf_list (pis);

      // If the (project-based) fallback system package name is specified,
      // then choose between the guessed and fallback names depending on which
      // of them is known to the system package manager.
      //
      // Specifically, if the guessed system package exists we use that.
      // Otherwise, if the fallback system package exists we use that and fail
      // otherwise.
      //
      if (!ps.fallback.empty ())
      {
        assert (pis.size () > 1); // devel, fallback,... or main, fallback,...

        // Either devel or main is guessed.
        //
        bool guessed_devel (!ps.devel.empty ());
        assert (guessed_devel == ps.main.empty ());

        string& guessed (guessed_devel ? ps.devel : ps.main);

        package_info& gi (pis[0]); // Guessed package info.
        package_info& fi (pis[1]); // Fallback package info.

        if (gi.unknown ())
        {
          if (fi.known ())
          {
            guessed = move (ps.fallback);
            gi = move (fi);
          }
          else
          {
            fail << "unable to guess " << (guessed_devel ? "devel" : "main")
                 << ' ' << os_release_.name_id << " package for " << pn <<
              info << "neither " << guessed << " nor " << ps.fallback
                   << ' ' << os_release_.name_id << " package exists" <<
              info << "consider specifying explicit mapping in " << pn
                   << " package manifest";

          }
        }

        // Whether it was used or not, cleanup the fallback information.
        //
        ps.fallback.clear ();
        pis.erase (pis.begin () + 1);
        --ps.package_infos_main;
      }
    }

    optional<package_status> r;

    {
      diag_record dr; // Ambiguity diagnostics.

      for (package_status& ps: candidates)
      {
        vector<package_info>& pis (ps.package_infos);

        // Handle the unknown main package.
        //
        if (ps.main.empty ())
        {
          const package_info& devel (pis.front ());

          // Note that at this stage we can only use the installed -devel
          // package (since the candidate version may change after fetch).
          //
          if (devel.installed_version.empty ())
            continue;

          guess_main (ps, devel.installed_version, devel.installed_arch);
          pis.emplace (pis.begin (), ps.main);
          ps.package_infos_main++;
          dnf_list (pis, 1);
        }

        optional<status_type> s (status (pis, ps.package_infos_main));

        if (!s || *s != package_status::installed)
          continue;

        const package_info& main (pis.front ());

        ps.status = *s;
        ps.system_name = main.name;
        ps.system_version = main.installed_version;

        if (!r)
        {
          r = move (ps);
          continue;
        }

        if (dr.empty ())
        {
          dr << fail << "multiple installed " << os_release_.name_id
             << " packages for " << pn <<
            info << "candidate: " << r->main << " " << r->system_version;
        }

        dr << info << "candidate: " << ps.main << " " << ps.system_version;
      }

      if (!dr.empty ())
        dr << info << "consider specifying the desired version manually";
    }

    // Next look for available versions if we are allowed to install.
    //
    if (!r && install_)
    {
      // If we weren't instructed to fetch or we already fetched, then we
      // don't need to re-run dnf_list().
      //
      bool requery;
      if ((requery = fetch_ && !fetched_))
      {
        dnf_makecache ();
        fetched_ = true;
      }

      {
        diag_record dr; // Ambiguity diagnostics.

        for (package_status& ps: candidates)
        {
          vector<package_info>& pis (ps.package_infos);

          if (requery)
            dnf_list (pis);

          // Handle the unknown main package.
          //
          if (ps.main.empty ())
          {
            const package_info& devel (pis.front ());

            // Note that this time we use the candidate version.
            //
            if (devel.candidate_version.empty ())
              continue; // Not installable.

            guess_main (ps, devel.candidate_version, devel.candidate_arch);
            pis.emplace (pis.begin (), ps.main);
            ps.package_infos_main++;
            dnf_list (pis, 1);
          }

          optional<status_type> s (status (pis, ps.package_infos_main));

          if (!s)
          {
            ps.main.clear (); // Not installable.
            continue;
          }

          assert (*s != package_status::installed); // Sanity check.

          const package_info& main (pis.front ());

          // Note that if we are installing something for this main package,
          // then we always go for the candidate version even though it may
          // have an installed version that may be good enough (especially if
          // what we are installing are extras). The reason is that it may as
          // well not be good enough (especially if we are installing the -dev
          // package) and there is no straightforward way to change our mind.
          //
          ps.status = *s;
          ps.system_name = main.name;
          ps.system_version = main.candidate_version;

          // Prefer partially installed to not installed. This makes detecting
          // ambiguity a bit trickier so we handle partially installed here
          // and not installed in a separate loop below.
          //
          if (ps.status != package_status::partially_installed)
            continue;

          if (!r)
          {
            r = move (ps);
            continue;
          }

          auto print_missing = [&dr] (const package_status& s)
          {
            for (const package_info& pi: s.package_infos)
              if (pi.installed_version.empty ())
                dr << ' ' << pi.name;
          };

          if (dr.empty ())
          {
            dr << fail << "multiple partially installed "
               << os_release_.name_id << " packages for " << pn;

            dr << info << "candidate: " << r->main << " " << r->system_version
               << ", missing components:";
            print_missing (*r);
          }

          dr << info << "candidate: " << ps.main << " " << ps.system_version
             << ", missing components:";
          print_missing (ps);
        }

        if (!dr.empty ())
          dr << info << "consider fully installing the desired package "
             << "manually and retrying the bpkg command";
      }

      if (!r)
      {
        diag_record dr; // Ambiguity diagnostics.

        for (package_status& ps: candidates)
        {
          if (ps.main.empty ())
            continue;

          assert (ps.status == package_status::not_installed); // Sanity check.

          if (!r)
          {
            r = move (ps);
            continue;
          }

          if (dr.empty ())
          {
            dr << fail << "multiple available " << os_release_.name_id
               << " packages for " << pn <<
              info << "candidate: " << r->main << " " << r->system_version;
          }

          dr << info << "candidate: " << ps.main << " " << ps.system_version;
        }

        if (!dr.empty ())
          dr << info << "consider installing the desired package manually and "
             << "retrying the bpkg command";
      }
    }

    if (r)
    {
      // Map the Fedora version to the bpkg version. But first strip the
      // revision from Fedora version ([<epoch>:]<version>-<release>).
      //
      // Note that according to deb-version(5), <upstream> may contain `:`/`-`
      // but in these cases <epoch>/<revision> must be specified explicitly,
      // respectively.
      //
      string sv (r->system_version, 0, r->system_version.rfind ('-'));

      optional<version> v (
        downstream_package_version (sv,
                                    *aps,
                                    os_release_.name_id,
                                    os_release_.version_id,
                                    os_release_.like_ids));

      if (!v)
      {
        // Fallback to using system version as downstream version. But first
        // strip the epoch, if any.
        //
        size_t p (sv.find (':'));
        if (p != string::npos)
          sv.erase (0, p + 1);

        try
        {
          v = version (sv);
        }
        catch (const invalid_argument& e)
        {
          fail << "unable to map " << os_release_.name_id << " package "
               << r->system_name << " version " << sv << " to bpkg package "
               << pn << " version" <<
            info << os_release_.name_id << " version is not a valid bpkg "
                 << "version: " << e.what () <<
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

  void system_package_manager_fedora::
  pkg_install (const vector<package_name>& pns)
  {
    assert (!pns.empty ());

    assert (install_ && !installed_);
    installed_ = true;

    // Collect and merge all the Fedora packages/version for the specified
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

      const package_status& ps (*it->second);

      // @@ Amend.
      //
      // At first it may seem we don't need to do anything for already fully
      // installed packages. But it's possible some of them were automatically
      // installed, meaning that they can be automatically removed if they no
      // longer have any dependents (see dnf(8) for details). Which in
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

      for (const package_info& pi: ps.package_infos)
      {
        string n (pi.name);
        string v (fi ? pi.installed_version : string ());

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
      // Convert to the `dnf install` <pkg>[-<ver>] form.
      //
      strings specs;
      specs.reserve (pkgs.size ());
      for (const package& p: pkgs)
      {
        string s (p.name);
        if (!p.version.empty ())
        {
          s += '-';
          s += p.version;
        }
        specs.push_back (move (s));
      }

      dnf_install (specs);
    }

    // Verify that versions we have promised in pkg_status() match what
    // actually got installed.
    //
    {
      vector<package_info> pis;

      // Here we just check the main package component of each package.
      //
      for (const package_name& pn: pns)
      {
        const package_status& ps (*status_cache_.find (pn)->second);

        if (find_if (pis.begin (), pis.end (),
                     [&ps] (const package_info& pi)
                     {
                       return pi.name == ps.system_name;
                     }) == pis.end ())
        {
          pis.push_back (package_info (ps.system_name));
        }
      }

      dnf_list (pis);

      for (const package_name& pn: pns)
      {
        const package_status& ps (*status_cache_.find (pn)->second);

        auto i (find_if (pis.begin (), pis.end (),
                         [&ps] (const package_info& pi)
                         {
                           return pi.name == ps.system_name;
                         }));
        assert (i != pis.end ());

        const package_info& pi (*i);

        if (pi.installed_version != ps.system_version)
        {
          fail << "unexpected " << os_release_.name_id << " package version "
               << "for " << ps.system_name <<
            info << "expected: " << ps.system_version <<
            info << "installed: " << pi.installed_version <<
            info << "consider retrying the bpkg command";
        }
      }
    }
  }
}
