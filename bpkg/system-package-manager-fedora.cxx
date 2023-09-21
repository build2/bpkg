// file      : bpkg/system-package-manager-fedora.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager-fedora.hxx>

#include <locale>

#include <bpkg/diagnostics.hxx>

#include <bpkg/pkg-bindist-options.hxx>

using namespace butl;

namespace bpkg
{
  using package_status = system_package_status_fedora;

  // Translate host CPU to Fedora package architecture.
  //
  string system_package_manager_fedora::
  arch_from_target (const target_triplet& h)
  {
    const string& c (h.cpu);
    return
      c == "i386" || c == "i486" || c == "i586" || c == "i686" ? "i686" :
      c;
  }

  // Parse the fedora-name (or alike) value. The first argument is the package
  // type.
  //
  // Note that for now we treat all the packages from the non-main groups as
  // extras omitting the -common package (assuming it's pulled by the main
  // package) as well as -doc and -debug* unless requested with the
  // extra_{doc,debug*} arguments. Note that we treat -static as -devel (since
  // we can't know whether the static library is needed or not).
  //
  package_status system_package_manager_fedora::
  parse_name_value (const string& pt,
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

    auto parse_group = [&split, &suffix] (const string& g, const string* pt)
    {
      strings ns (split (g, ' '));

      if (ns.empty ())
        fail << "empty package group";

      package_status r;

      // Handle the "devel instead of main" special case for libraries.
      //
      // Check that the following name does not end with -devel. This will be
      // the only way to disambiguate the case where the library name happens
      // to end with -devel (e.g., libfoo-devel libfoo-devel-devel).
      //
      {
        string& m (ns[0]);

        if (pt != nullptr        &&
            *pt == "lib"         &&
            suffix (m, "-devel") &&
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
                         suffix (n, (w = "-static"))      ? &r.static_     :
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
        r = parse_group (gs[i], &pt);
      else
      {
        package_status g (parse_group (gs[i], nullptr));

        if (!g.main.empty ())             r.extras.push_back (move (g.main));
        if (!g.devel.empty ())            r.extras.push_back (move (g.devel));
        if (!g.static_.empty ())          r.extras.push_back (move (g.static_));
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
  // on the extracted (by dnf_repoquery_requires()) dependencies, passed as a
  // list of the package name/version pairs. Return empty string if unable to.
  //
  string system_package_manager_fedora::
  main_from_devel (const string& devel_name,
                   const string& devel_ver,
                   const vector<pair<string, string>>& depends)
  {
    // For the main package we first look for a dependency with the
    // <devel-stem>-libs name and the devel_ver version. Failed that, we try
    // just <devel-stem>.
    //
    // Note that the order is important since for a mixed package we need to
    // end up with the -libs sub-package rather than with the base package as,
    // for example, in the following case:
    //
    // sqlite-devel  3.36.0-3.fc35 ->
    //   sqlite      3.36.0-3.fc35
    //   sqlite-libs 3.36.0-3.fc35
    //
    string devel_stem (devel_name, 0, devel_name.rfind ("-devel"));

    auto find = [&devel_ver, &depends] (const string& n)
    {
      auto i (find_if (depends.begin (), depends.end (),
                       [&n, &devel_ver] (const pair<string, string>& d)
                       {
                         return d.first == n && d.second == devel_ver;
                       }));

      return i != depends.end () ? i->first : string ();
    };

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
    // error diagnostics (try specifying a single unknown package).
    //
    cstrings args {
      "dnf", "list",
      "--all",       // Look for both installed and available.
      "--cacheonly", // Don't automatically update the metadata.
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

    // Note that `dnf list` fails if there are no matching packages to print.
    // Thus, let's hack around this by adding the rpm package to the list, so
    // that at least one package is always present and the command can never
    // fail for that reason.
    //
    // Also note that we still allow the rpm package to appear in the
    // specified package list.
    //
    bool rpm (false);
    args.push_back ("rpm");

    args.push_back (nullptr);

    // Run with the C locale to make sure there is no localization.
    //
    const char* evars[] = {"LC_ALL=C", nullptr};

    try
    {
      if (dnf_path.empty () && !simulate_)
        dnf_path = process::path_search (args[0], false /* init */);

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
        strings k;
        for (size_t i (0); i != n; ++i)
          k.push_back (pis[i].name);

        const path* f (nullptr);
        if (installed_)
        {
          auto i (simulate_->dnf_list_installed_.find (k));
          if (i != simulate_->dnf_list_installed_.end ())
            f = &i->second;
        }
        if (f == nullptr && fetched_)
        {
          auto i (simulate_->dnf_list_fetched_.find (k));
          if (i != simulate_->dnf_list_fetched_.end ())
            f = &i->second;
        }
        if (f == nullptr)
        {
          auto i (simulate_->dnf_list_.find (k));
          if (i != simulate_->dnf_list_.end ())
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
      }

      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

        // The output of `dnf list <pkg1> <pkg2> ...` is the 2 groups of lines
        // in the following form:
        //
        // Installed Packages
        // <pkg1>.<arch1>            13.0.0-3.fc35        @<repo1>
        // <pkg2>.<arch2>            69.1-6.fc35          @<repo2>
        // Available Packages
        // <pkg1>.<arch1>            13.0.1-1.fc35        <repo1>
        // <pkg3>.<arch3>            1.2.11-32.fc35       <repo3>
        //
        // Where unknown packages are omitted. The lines order does not
        // necessarily match the order of the packages on the command line.
        // It looks like there should be not blank lines but who really knows.
        //
        // Note also that if a package appears in the 'Installed Packages'
        // group, then it only appears in the 'Available Packages' if the
        // candidate version is better. Only the single (best) available
        // version is listed, which we call the candidate version.
        //
        {
          auto df = make_diag_frame (
            [&pe, &args] (diag_record& dr)
            {
              dr << info << "while parsing output of ";
              print_process (dr, pe, args);
            });

          // Keep track of whether we are inside of the 'Installed Packages'
          // or 'Available Packages' sections.
          //
          optional<bool> installed;

          for (string l; !eof (getline (is, l)); )
          {
            if (l == "Installed Packages")
            {
              if (installed)
                fail << "unexpected line '" << l << "'";

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

            // Parse the package name.
            //
            size_t e (l.find (' '));

            if (l.empty () || e == 0)
              fail << "expected package name in '" << l << "'";

            if (e == string::npos)
              fail << "expected package version in '" << l << "'";

            string p (l, 0, e);

            // Parse the package version.
            //
            size_t b (l.find_first_not_of (' ', e + 1));

            if (b == string::npos)
              fail << "expected package version in '" << l << "'";

            // It doesn't not seem that the repository id can be absent. Even
            // if the package is installed manually it is assumed to come from
            // some special repository (@commandline, etc). For example:
            //
            // # dnf install ./libsigc++30-3.0.7-2.fc35.x86_64.rpm
            // # rpm -i ./libsigc++30-devel-3.0.7-2.fc35.x86_64.rpm
            // # dnf list --quiet libsigc++30.x86_64 libsigc++30-devel.x86_64
            // Installed Packages
            // libsigc++30.x86_64        3.0.7-2.fc35  @@commandline
            // libsigc++30-devel.x86_64  3.0.7-2.fc35  @@System
            //
            // Thus, we assume that the version is always followed with the
            // space character.
            //
            e = l.find (' ', b + 1);

            if (e == string::npos)
              fail << "expected package repository in '" << l << "'";

            string v (l, b, e - b);

            // While we don't really care about the rest of the line, let's
            // verify that it contains a repository id, for good measure.
            //
            b = l.find_first_not_of (' ', e + 1);

            if (b == string::npos)
              fail << "expected package repository in '" << l << "'";

            // Separate the architecture from the package name.
            //
            e = p.rfind ('.');

            if (e == string::npos || e == 0 || e == p.size () - 1)
              fail << "can't extract architecture for package " << p
                   << " in '" << l << "'";

            string a (p, e + 1);

            // Skip the package if its architecture differs from the host
            // architecture.
            //
            if (a != arch && a != "noarch")
              continue;

            p.resize (e);

            if (p == "rpm")
              rpm = true;

            // Find the package info to update.
            //
            auto i (find_if (pis.begin (), pis.end (),
                             [&p] (const package_info& pi)
                             {return pi.name == p;}));

            if (i == pis.end ())
            {
              // Skip the special rpm package which may not be present in the
              // list.
              //
              if (p == "rpm")
                continue;

              fail << "unexpected package " << p << '.' << a << ' ' << v
                   << " in '" << l << "'";
            }

            string& ver (*installed
                         ? i->installed_version
                         : i->candidate_version);

            if (!ver.empty ())
              fail << "multiple " << (*installed ? "installed " : "available ")
                   << "versions of package " << p << '.' << a <<
                info << "version: " << ver <<
                info << "version: " << v;

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

    if (!rpm)
      fail << "rpm package doesn't exist";

    // Note that if a Fedora package is installed but the repository doesn't
    // contain a better version, then this package won't appear in the
    // 'Available Packages' section of the `dnf list` output and thus the
    // candidate_version will stay empty. Let's set it to the installed
    // version in this case to be consistent with the Debian's semantics and
    // keep the Fedora and Debian system package manager implementations
    // aligned.
    //
    for (size_t i (0); i != n; ++i)
    {
      package_info& pi (pis[i]);

      if (pi.candidate_version.empty () && !pi.installed_version.empty ())
      {
        pi.candidate_version = pi.installed_version;
        pi.candidate_arch = pi.installed_arch;
      }
    }
  }

  // Execute `dnf repoquery --requires` for the specified
  // package/version/architecture and return its dependencies as a list of the
  // name/version pairs.
  //
  // It is expected that the specified package/version/architecture is known
  // (e.g., returned by the `dnf list` command). Note that if that's not the
  // case (can happen due to a race), then an empty list is returned. This,
  // however, is ok for our current usage since in this case we will shortly
  // fail with the 'unable to guess main package' error anyway.
  //
  // Note that the returned dependencies are always of the host architecture
  // or noarch. For example:
  //
  // dhcp-client-12:4.4.3-4.P1.fc35.x86_64 ->
  //   dhcp-common-12:4.4.3-4.P1.fc35.noarch
  //   coreutils-8.32-36.fc35.x86_64
  //   ...
  //
  // rust-uuid+std-devel-1.2.1-1.fc35.noarch ->
  //   rust-uuid-devel-1.2.1-1.fc35.noarch
  //   cargo-1.65.0-1.fc35.x86_64
  //
  vector<pair<string, string>> system_package_manager_fedora::
  dnf_repoquery_requires (const string& name,
                          const string& ver,
                          const string& qarch,
                          bool installed)
  {
    assert (!name.empty () && !ver.empty () && !arch.empty ());

    // Qualify the package with the architecture suffix.
    //
    // Note that for reasons unknown, the below command may still print some
    // dependencies with different architecture (see the below example). It
    // feels sensible to just skip them.
    //
    string spec (name + '-' + ver + '.' + qarch);

    // The --quiet option makes sure we don't get 'Last metadata expiration
    // check: <timestamp>' printed to stderr. It does not appear to affect
    // error diagnostics (try specifying an unknown option).
    //
    cstrings args {
      "dnf", "repoquery", "--requires",
      "--quiet",
      "--cacheonly", // Don't automatically update the metadata.
      "--resolve",   // Resolve requirements to packages/versions.
      "--qf", "%{name} %{arch} %{epoch}:%{version}-%{release}"};

    // Note that installed packages which are not available from configured
    // repositories (e.g. packages installed from local rpm files or temporary
    // local repositories, package versions not available anymore from their
    // original repositories, etc) are not seen by `dnf repoquery` by
    // default. It also turned out that the --installed option not only limits
    // the resulting set to the installed packages, but also makes `dnf
    // repoquery` to see all the installed packages, including the unavailable
    // ones. Thus, we always add this option to query dependencies of the
    // installed packages.
    //
    if (installed)
    {
      args.push_back ("--installed");

      // dnf(8) also recommends to use --disableexcludes together with
      // --install to make sure that all installed packages will be listed and
      // no configuration file may influence the result.
      //
      args.push_back ("--disableexcludes=all");
    }

    args.push_back (spec.c_str ());
    args.push_back (nullptr);

    // Note that for this command there seems to be no need to run with the C
    // locale since the output is presumably not localizable. But let's do it
    // for good measure.
    //
    const char* evars[] = {"LC_ALL=C", nullptr};

    vector<pair<string, string>> r;
    try
    {
      if (dnf_path.empty () && !simulate_)
        dnf_path = process::path_search (args[0], false /* init */);

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
        simulation::package k {name, ver, qarch, installed};

        const path* f (nullptr);
        if (fetched_)
        {
          auto i (simulate_->dnf_repoquery_requires_fetched_.find (k));
          if (i != simulate_->dnf_repoquery_requires_fetched_.end ())
            f = &i->second;
        }
        if (f == nullptr)
        {
          auto i (simulate_->dnf_repoquery_requires_.find (k));
          if (i != simulate_->dnf_repoquery_requires_.end ())
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
      }

      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

        // The output of the command will be the sequence of the package lines
        // in the `<name> <arc> <version>` form (per the -qf option above). So
        // for example for the libicu-devel-69.1-6.fc35.x86_64 package it is
        // as follows:
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
        // Note that there is also a self-dependency.
        //
        for (string l; !eof (getline (is, l)); )
        {
          // Parse the package name.
          //
          size_t e (l.find (' '));

          if (l.empty () || e == 0)
            fail << "expected package name in '" << l << "'";

          if (e == string::npos)
            fail << "expected package architecture in '" << l << "'";

          string p (l, 0, e);

          // Parse the package architecture.
          //
          size_t b (e + 1);
          e = l.find (' ', b);

          if (e == string::npos)
            fail << "expected package version in '" << l << "'";

          string a (l, b, e - b);
          if (a.empty ())
            fail << "expected package architecture in '" << l << "'";

          // Parse the package version.
          //
          string v (l, e + 1);

          // Strip the '0:' epoch from the package version to align with
          // versions retrieved by other functions (dnf_list(), etc).
          //
          e = v.find (':');
          if (e == string::npos || e == 0)
            fail << "no epoch for package version in '" << l << "'";

          if (e == 1 && v[0] == '0')
            v.erase (0, 2);

          // Skip a potential self-dependency and dependencies of a different
          // architecture.
          //
          if (p == name || (a != arch && a != "noarch"))
            continue;

          r.emplace_back (move (p), move (v));
        }

        is.close ();
      }
      catch (const io_error& e)
      {
        if (pr.wait ())
          fail << "unable to read " << args[0] << " repoquery --requires "
               << "output: " << e;

        // Fall through.
      }

      if (!pr.wait ())
      {
        diag_record dr (fail);
        dr << args[0] << " repoquery --requires exited with non-zero code";

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

  // Prepare the common options for commands which update the system.
  //
  pair<cstrings, const process_path&> system_package_manager_fedora::
  dnf_common (const char* command,
              optional<size_t> fetch_timeout,
              strings& args_storage)
  {
    // Pre-allocate the required number of entries in the arguments storage.
    //
    if (fetch_timeout)
      args_storage.reserve (1);

    cstrings args;

    if (!sudo_.empty ())
      args.push_back (sudo_.c_str ());

    args.push_back ("dnf");
    args.push_back (command);

    // Map our verbosity/progress to dnf --quiet and --verbose options.
    //
    // Note that all the diagnostics, including the progress indication and
    // general information (like what's being installed) but excluding error
    // messages, is printed to stdout. So we fix this by redirecting stdout to
    // stderr. By default the progress bar for network transfers is printed,
    // unless stdout is not a terminal. The --quiet option disables printing
    // the plan and all the progress indication, but not the confirmation
    // prompt nor error messages.
    //
    if (progress_ && *progress_)
    {
      // Print the progress bar by default, unless this is not a terminal
      // (there is no way to force it).
    }
    else if (verb == 0 || (progress_ && !*progress_))
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

    // Add the network operations timeout configuration options, if requested.
    //
    if (fetch_timeout)
    {
      args_storage.push_back (
        "--setopt=timeout=" + to_string (*fetch_timeout));

      args.push_back (args_storage.back ().c_str ());
      args.push_back ("--setopt=minrate=0");
    }

    try
    {
      const process_path* pp (nullptr);

      if (!sudo_.empty ())
      {
        if (sudo_path.empty () && !simulate_)
          sudo_path = process::path_search (args[0], false /* init */);

        pp = &sudo_path;
      }
      else
      {
        if (dnf_path.empty () && !simulate_)
          dnf_path = process::path_search (args[0], false /* init */);

        pp = &dnf_path;
      }

      return pair<cstrings, const process_path&> (move (args), *pp);
    }
    catch (const process_error& e)
    {
      fail << "unable to execute " << args[0] << ": " << e << endf;
    }
  }

  // Execute `dnf makecache` to download and cache the repositories metadata.
  //
  void system_package_manager_fedora::
  dnf_makecache ()
  {
    strings args_storage;
    pair<cstrings, const process_path&> args_pp (
      dnf_common ("makecache", fetch_timeout_, args_storage));

    cstrings& args (args_pp.first);
    const process_path& pp (args_pp.second);

    args.push_back ("--refresh");
    args.push_back (nullptr);

    try
    {
      if (verb >= 2)
        print_process (args);
      else if (verb == 1)
        text << "updating " << os_release.name_id << " repositories metadata...";

      process pr;
      if (!simulate_)
      {
        // Redirect stdout to stderr.
        //
        pr = process (pp, args, 0 /* stdin */, 2 /* stdout */);
      }
      else
      {
        print_process (args);
        pr = process (process_exit (simulate_->dnf_makecache_fail_ ? 1 : 0));
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
        text << "updated " << os_release.name_id << " repositories metadata";
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  // Execute `dnf install` to install the specified packages (e.g.,
  // libfoo.x86_64 or libfoo-1.2.3-1.fc35.x86_64).
  //
  // Note that the package name can only contain alpha-numeric characters,
  // '-', '.', '_', and '+' (see Guidelines for Naming Fedora Packages for
  // details). If specified, both the version (1.2.3) and release (1.fc35)
  // parts are mandatory and may only contain alpha-numeric characters, `.`,
  // `_`, `+`, `~`, and `^` (see the RPM spec file format documentation for
  // details). Thus, package specs (which are actually wildcards) are
  // generally ambiguous, so that libfoo-1.2.3-1.fc35.x86_64 may theoretically
  // be a package name and libfoo-bar a specific package version.
  //
  // By default, `dnf install` tries to interpret the spec as the
  // <name>-[<epoch>:]<version>-<release>.<arch> form prior to trying the
  // <name>.<arch> form until any matched packages are found (see SPECIFYING
  // PACKAGES section of dnf(8) for more details on the spec matching
  // rules). We could potentially use `dnf install-nevra` command for the
  // package version specs and `dnf install-na` for the package name specs.
  // Let's, however, keep it simple for now given that clashes for our
  // use-case are presumably not very likely.
  //
  void system_package_manager_fedora::
  dnf_install (const strings& pkgs)
  {
    assert (!pkgs.empty ());

    strings args_storage;
    pair<cstrings, const process_path&> args_pp (
      dnf_common ("install", fetch_timeout_, args_storage));

    cstrings& args (args_pp.first);
    const process_path& pp (args_pp.second);

    // Note that we can't use --cacheonly here to prevent the metadata update,
    // since the install command then expects the package RPM files to also be
    // cached and fails if that's not the case. Thus we have to override the
    // metadata_expire=never configuration option instead. Which makes the
    // whole thing quite hairy and of dubious value -- there is nothing wrong
    // with letting it re-fetch the metadata during install (which in fact may
    // save us from attempting to download no longer existing packages).
    //
#if 0
    args.push_back ("--setopt=metadata_expire=never");
#endif

    for (const string& p: pkgs)
      args.push_back (p.c_str ());

    args.push_back (nullptr);

    try
    {
      if (verb >= 2)
        print_process (args);
      else if (verb == 1)
        text << "installing " << os_release.name_id << " packages...";

      process pr;
      if (!simulate_)
      {
        // Redirect stdout to stderr.
        //
        pr = process (pp, args, 0 /* stdin */, 2 /* stdout */);
      }
      else
      {
        print_process (args);
        pr = process (process_exit (simulate_->dnf_install_fail_ ? 100 : 0));
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

      if (verb == 1)
        text << "installed " << os_release.name_id << " packages";
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  // Execute `dnf mark install` to mark the installed packages as installed by
  // the user (see dnf_install() for details on the package specs).
  //
  // Note that an installed package may be marked as installed by the user
  // rather than as a dependency. In particular, such a package will never be
  // automatically removed as an unused dependency. This mark can be added and
  // removed by the `dnf mark install` and `dnf mark remove` commands,
  // respectively. Besides that, this mark is automatically added by `dnf
  // install` for a package specified on the command line, but only if it is
  // not yet installed. Note that this mark will not be added automatically
  // for an already installed package even if it is upgraded explicitly. For
  // example:
  //
  // $ sudo dnf install libsigc++30-devel-3.0.2-2.fc32 --repofrompath test,./repo --setopt=gpgcheck=0 --assumeyes
  // Installed: libsigc++30-3.0.2-2.fc32.x86_64 libsigc++30-devel-3.0.2-2.fc32.x86_64
  //
  // $ sudo dnf install --best libsigc++30 --assumeyes
  // Upgraded: libsigc++30-3.0.7-2.fc35.x86_64 libsigc++30-devel-3.0.7-2.fc35.x86_64
  //
  // $ sudo dnf remove libsigc++30-devel --assumeyes
  // Removed: libsigc++30-3.0.7-2.fc35.x86_64 libsigc++30-devel-3.0.7-2.fc35.x86_64
  //
  void system_package_manager_fedora::
  dnf_mark_install (const strings& pkgs)
  {
    assert (!pkgs.empty ());

    strings args_storage;
    pair<cstrings, const process_path&> args_pp (
      dnf_common ("mark", nullopt /* fetch_timeout */, args_storage));

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
      {
        // Redirect stdout to stderr.
        //
        pr = process (pp, args, 0 /* stdin */, 2 /* stdout */);
      }
      else
      {
        print_process (args);
        pr = process (process_exit (simulate_->dnf_mark_install_fail_ ? 1 : 0));
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
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  optional<const system_package_status*> system_package_manager_fedora::
  status (const package_name& pn, const available_packages* aps)
  {
    // First check the cache.
    //
    {
      auto i (status_cache_.find (pn));

      if (i != status_cache_.end ())
        return i->second ? &*i->second : nullptr;

      if (aps == nullptr)
        return nullopt;
    }

    optional<package_status> r (status (pn, *aps));

    // Cache.
    //
    auto i (status_cache_.emplace (pn, move (r)).first);
    return i->second ? &*i->second : nullptr;
  }

  optional<package_status> system_package_manager_fedora::
  status (const package_name& pn, const available_packages& aps)
  {
    tracer trace ("system_package_manager_fedora::status");

    // For now we ignore -doc and -debug* package components (but we may want
    // to have options controlling this later). Note also that we assume
    // -common is pulled automatically by the base package so we ignore it as
    // well (see equivalent logic in parse_name_value()).
    //
    bool need_doc (false);
    bool need_debuginfo (false);
    bool need_debugsource (false);

    vector<package_status> candidates;

    // Translate our package name to the Fedora package names.
    //
    {
      auto df = make_diag_frame (
        [this, &pn] (diag_record& dr)
        {
          dr << info << "while mapping " << pn << " to " << os_release.name_id
             << " package name";
        });

      // Without explicit type, the best we can do in trying to detect whether
      // this is a library is to check for the lib prefix. Libraries without
      // the lib prefix and non-libraries with the lib prefix (both of which
      // we do not recomment) will have to provide a manual mapping (or
      // explicit type).
      //
      // Note that using the first (latest) available package as a source of
      // type information seems like a reasonable choice.
      //
      const string& pt (!aps.empty ()
                        ? aps.front ().first->effective_type ()
                        : package_manifest::effective_type (nullopt, pn));

      strings ns;
      if (!aps.empty ())
        ns = system_package_names (aps,
                                   os_release.name_id,
                                   os_release.version_id,
                                   os_release.like_ids,
                                   true /* native */);
      if (ns.empty ())
      {
        // Attempt to automatically translate our package name. Failed that we
        // should try to use the project name, if present, as a fallback.
        //
        const string& n (pn.string ());

        // Note that theoretically different available packages can have
        // different project names. But taking it from the latest version
        // feels good enough.
        //
        const shared_ptr<available_package>& ap (!aps.empty ()
                                                 ? aps.front ().first
                                                 : nullptr);

        string f (ap != nullptr && ap->project && *ap->project != pn
                  ? ap->project->string ()
                  : empty_string);

        if (pt == "lib")
        {
          // If there is no project name let's try to use the package name
          // with the lib prefix stripped as a fallback. Note that naming
          // library packages without the lib prefix is quite common in Fedora
          // (xerces-c, uuid-c++, etc).
          //
          if (f.empty ())
            f = string (n, 3);

          f += "-devel";

          // Keep the base package name empty as an indication that it is to
          // be discovered.
          //
          candidates.push_back (package_status ("", n + "-devel", move (f)));
        }
        else
          candidates.push_back (package_status (n, "", move (f)));
      }
      else
      {
        // Parse each manual mapping.
        //
        for (const string& n: ns)
        {
          package_status s (parse_name_value (pt,
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
             // Should we verify the rest matches for good measure? But what
             // if we need to override, as in:
             //
             // fedora_35-name: libfoo libfoo-bar-devel
             // fedora_34-name: libfoo libfoo-devel
             //
             // Note that for this to work we must get fedora_35 values before
             // fedora_34, which is the semantics guaranteed by
             // system_package_names().
          }
        }
      }
    }

    // Guess unknown main package given the -devel package, its version, and
    // architecture. Failed that, assume the package to be a binless library
    // and leave the main member of the package_status object empty.
    //
    auto guess_main = [this, &trace] (package_status& s,
                                      const string& ver,
                                      const string& qarch,
                                      bool installed)
    {
      vector<pair<string, string>> depends (
        dnf_repoquery_requires (s.devel, ver, qarch, installed));

      s.main = main_from_devel (s.devel, ver, depends);

      if (s.main.empty ())
      {
        if (verb >= 4)
        {
          diag_record dr (trace);
          dr << "unable to guess main package for " << s.devel << ' ' << ver;

          if (!depends.empty ())
          {
            dr << ", depends on";

            for (auto b (depends.begin ()), i (b); i != depends.end (); ++i)
              dr << (i == b ? " " : ", ") << i->first << ' ' << i->second;
          }
          else
            dr << ", has no dependencies";
        }
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
    optional<package_status> r;

    {
      diag_record dr; // Ambiguity diagnostics.

      for (package_status& ps: candidates)
      {
        vector<package_info>& pis (ps.package_infos);

        // Query both main and fallback packages with a single dns_list()
        // invocation.
        //
        if (!ps.main.empty ())            pis.emplace_back (ps.main);
        if (!ps.devel.empty ())           pis.emplace_back (ps.devel);
        if (!ps.fallback.empty ())        pis.emplace_back (ps.fallback);
        if (!ps.static_.empty ())         pis.emplace_back (ps.static_);
        if (!ps.doc.empty () && need_doc) pis.emplace_back (ps.doc);

        if (!ps.debuginfo.empty () && need_debuginfo)
          pis.emplace_back (ps.debuginfo);

        if (!ps.debugsource.empty () && need_debugsource)
          pis.emplace_back (ps.debugsource);

        if (!ps.common.empty () && false) pis.emplace_back (ps.common);
        ps.package_infos_main = pis.size ();
        for (const string& n: ps.extras)  pis.emplace_back (n);

        dnf_list (pis);

        // Handle the fallback package name, if specified.
        //
        // Specifically, if the main/devel package is known to the system
        // package manager we use that. Otherwise, if the fallback package is
        // known we use that. And if neither is known, then we skip this
        // candidate (ps).
        //
        if (!ps.fallback.empty ())
        {
          assert (pis.size () > 1); // devel+fallback or main+fallback

          package_info& mp (pis[0]); // Main/devel package info.
          package_info& fp (pis[1]); // Fallback package info.

          // Note that at this stage we can only use the installed main/devel
          // and fallback packages (since the candidate versions may change
          // after fetch).
          //
          // Also note that this logic prefers installed fallback package to
          // potentially available non-fallback package.
          //
          if (mp.installed_version.empty ())
          {
            if (!fp.installed_version.empty ())
            {
              // Use the fallback.
              //
              (ps.main.empty () ? ps.devel : ps.main) = move (ps.fallback);
              mp = move (fp);
            }
            else
              continue; // Skip the candidate at this stage.
          }

          // Whether it was used or not, cleanup the fallback information.
          //
          ps.fallback.clear ();
          pis.erase (pis.begin () + 1);
          --ps.package_infos_main;
        }

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

          guess_main (ps,
                      devel.installed_version,
                      devel.installed_arch,
                      true /* installed */);

          if (!ps.main.empty ()) // Not a binless library?
          {
            pis.emplace (pis.begin (), ps.main);
            ps.package_infos_main++;
            dnf_list (pis, 1);
          }
        }

        optional<status_type> s (status (pis, ps.package_infos_main));

        if (!s || *s != package_status::installed)
          continue;

        const package_info& main (pis.front ()); // Main/devel.

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
          dr << fail << "multiple installed " << os_release.name_id
             << " packages for " << pn <<
            info << "candidate: " << r->system_name << ' ' << r->system_version;
        }

        dr << info << "candidate: " << ps.system_name << ' '
           << ps.system_version;
      }

      if (!dr.empty ())
        dr << info << "consider specifying the desired version manually";
    }

    // Next look for available versions if we are allowed to install. Indicate
    // the non-installable candidates by setting both their main and -devel
    // package names to empty strings.
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

          // Handle the fallback package name, if specified.
          //
          if (!ps.fallback.empty ())
          {
            assert (pis.size () > 1); // devel+fallback or main+fallback

            package_info& mp (pis[0]); // Main/devel package info.
            package_info& fp (pis[1]); // Fallback package info.

            // Note that this time we use the candidate versions.
            //
            if (mp.candidate_version.empty ())
            {
              string& main (!ps.main.empty () ? ps.main : ps.devel);

              if (!fp.candidate_version.empty ())
              {
                // Use the fallback.
                //
                main = move (ps.fallback);
                mp = move (fp);
              }
              else
              {
                // Otherwise, we would have resolved the name on the previous
                // stage.
                //
                assert (mp.installed_version.empty () &&
                        fp.installed_version.empty ());

                // Main/devel package is not installable.
                //
                main.clear ();
                continue;
              }
            }

            // Whether it was used or not, cleanup the fallback information.
            //
            ps.fallback.clear ();
            pis.erase (pis.begin () + 1);
            --ps.package_infos_main;
          }

          // Handle the unknown main package.
          //
          if (ps.main.empty ())
          {
            const package_info& devel (pis.front ());

            // Note that this time we use the candidate version.
            //
            if (devel.candidate_version.empty ())
            {
              // Not installable.
              //
              ps.devel.clear ();
              continue;
            }

            guess_main (ps,
                        devel.candidate_version,
                        devel.candidate_arch,
                        devel.candidate_version == devel.installed_version);

            if (!ps.main.empty ()) // Not a binless library?
            {
              pis.emplace (pis.begin (), ps.main);
              ps.package_infos_main++;
              dnf_list (pis, 1);
            }
          }

          optional<status_type> s (status (pis, ps.package_infos_main));

          if (!s)
          {
            // Not installable.
            //
            ps.main.clear ();
            ps.devel.clear ();
            continue;
          }

          assert (*s != package_status::installed); // Sanity check.

          const package_info& main (pis.front ()); // Main/devel.

          // Note that if we are installing something for this main package,
          // then we always go for the candidate version even though it may
          // have an installed version that may be good enough (especially if
          // what we are installing are extras). The reason is that it may as
          // well not be good enough (especially if we are installing the
          // -devel package) and there is no straightforward way to change our
          // mind.
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
               << os_release.name_id << " packages for " << pn;

            dr << info << "candidate: " << r->system_name << ' '
               << r->system_version << ", missing components:";
            print_missing (*r);
          }

          dr << info << "candidate: " << ps.system_name << ' '
             << ps.system_version << ", missing components:";
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
          if (ps.main.empty () && ps.devel.empty ()) // Not installable?
            continue;

          assert (ps.status == package_status::not_installed); // Sanity check.

          if (!r)
          {
            r = move (ps);
            continue;
          }

          if (dr.empty ())
          {
            dr << fail << "multiple available " << os_release.name_id
               << " packages for " << pn <<
              info << "candidate: " << r->system_name << ' '
                   << r->system_version;
          }

          dr << info << "candidate: " << ps.system_name << ' '
             << ps.system_version;
        }

        if (!dr.empty ())
          dr << info << "consider installing the desired package manually and "
             << "retrying the bpkg command";
      }
    }

    if (r)
    {
      // Map the Fedora version to the bpkg version. But first strip the
      // release from Fedora version ([<epoch>:]<version>-<release>).
      //
      string sv (r->system_version, 0, r->system_version.rfind ('-'));

      optional<version> v;
      if (!aps.empty ())
        v = downstream_package_version (sv,
                                        aps,
                                        os_release.name_id,
                                        os_release.version_id,
                                        os_release.like_ids);

      if (!v)
      {
        // Fallback to using system version as downstream version. But first
        // strip the epoch, if any. Also convert the potential pre-release
        // separator to the bpkg version pre-release separator.
        //
        size_t p (sv.find (':'));
        if (p != string::npos)
          sv.erase (0, p + 1);

        // Consider the first '~' character as a pre-release separator. Note
        // that if there are more of them, then we will fail since '~' is an
        // invalid character for bpkg version.
        //
        p = sv.find ('~');
        if (p != string::npos)
          sv[p] = '-';

        try
        {
          v = version (sv);
        }
        catch (const invalid_argument& e)
        {
          fail << "unable to map " << os_release.name_id << " package "
               << r->system_name << " version " << sv << " to bpkg package "
               << pn << " version" <<
            info << os_release.name_id << " version is not a valid bpkg "
                 << "version: " << e.what () <<
            info << "consider specifying explicit mapping in " << pn
                 << " package manifest";
        }
      }

      r->version = move (*v);
    }

    return r;
  }

  void system_package_manager_fedora::
  install (const vector<package_name>& pns)
  {
    assert (!pns.empty ());

    assert (install_ && !installed_);
    installed_ = true;

    // Collect and merge all the Fedora packages/versions for the specified
    // bpkg packages.
    //
    struct package
    {
      string name;
      string version; // Empty if unspecified.
      string arch;    // Always specified.
    };
    vector<package> pkgs;

    // At first it may seem we don't need to do anything for already fully
    // installed packages. But it's possible some of them were automatically
    // installed, meaning that they can be automatically removed if they no
    // longer have any dependents (see dnf(8) for details). Which in turn
    // means that things may behave differently depending on whether we've
    // installed a package ourselves or if it was already installed.
    //
    // So what we are going to do is to run `dnf install` only if there are
    // any non-fully installed packages. In this case we will pass all the
    // packages, including the fully installed ones. But we must be careful
    // not to force their upgrade. To achieve this we will specify the
    // installed version as the desired version. Whether we run `dnf install`
    // or not we will also always run `dnf mark install` afterwards for all
    // the packages to mark them as installed by the user.
    //
    // Note also that for partially/not installed we don't specify the
    // version, expecting the candidate version to be installed. We, however,
    // still specify the candidate architecture in this case, since for
    // reasons unknown dnf may install a package of a different architecture
    // otherwise.
    //
    bool install (false);

    for (const package_name& pn: pns)
    {
      auto it (status_cache_.find (pn));
      assert (it != status_cache_.end () && it->second);

      const package_status& ps (*it->second);
      bool fi (ps.status == package_status::installed);

      if (!fi)
        install = true;

      for (const package_info& pi: ps.package_infos)
      {
        string n (pi.name);
        string v (fi ? pi.installed_version : string ());
        string a (fi ? pi.installed_arch    : pi.candidate_arch);

        auto i (find_if (pkgs.begin (), pkgs.end (),
                         [&n] (const package& p)
                         {
                           return p.name == n;
                         }));

        if (i != pkgs.end ())
        {
          if (i->version.empty ())
          {
            i->version = move (v);
            i->arch = move (a);
          }
          else
            // Feels like this cannot happen since we always use the installed
            // version of the package.
            //
            assert (i->version == v && i->arch == a);
        }
        else
          pkgs.push_back (package {move (n), move (v), move (a)});
      }
    }

    // Convert to the <name>-[<epoch>:]<version>-<release>.<arch> package spec
    // for the installed packages and to the <name> spec for partially/not
    // installed ones (see dnf_install() for details on the package specs).
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
      s += '.';
      s += p.arch;

      specs.push_back (move (s));
    }

    // Install.
    //
    if (install)
      dnf_install (specs);

    // Mark as installed by the user.
    //
    dnf_mark_install (specs);

    // Verify that versions we have promised in status() match what actually
    // got installed.
    //
    if (install)
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
          fail << "unexpected " << os_release.name_id << " package version "
               << "for " << ps.system_name <<
            info << "expected: " << ps.system_version <<
            info << "installed: " << pi.installed_version <<
            info << "consider retrying the bpkg command";
        }
      }
    }
  }

  // Map non-system bpkg package to system package name(s) and version.
  //
  // This is used both to map the package being generated and its
  // dependencies. What should we do with extras returned in package_status?
  // We can't really generate any of them (which files would we place in
  // them?) nor can we list them as dependencies (we don't know their system
  // versions). So it feels like the only sensible choice is to ignore extras.
  //
  // In a sense, we have a parallel arrangement going on here: binary packages
  // that we generate don't have extras (i.e., they include everything
  // necessary in the "standard" packages from the main group) and when we
  // punch a system dependency based on a non-system bpkg package, we assume
  // it was generated by us and thus doesn't have any extras. Or, to put it
  // another way, if you want the system dependency to refer to a "native"
  // system package with extras you need to configure it as a system bpkg
  // package.
  //
  // In fact, this extends to package names. For example, unless custom
  // mapping is specified, we will generate libsqlite3 and libsqlite3-devel
  // while native names are sqlite-libs and sqlite-devel. While this duality
  // is not ideal, presumably we will normally only be producing our binary
  // packages if there are no suitable native packages. And for a few
  // exceptions (e.g., our package is "better" in some way, such as configured
  // differently or fixes a critical bug), we will just have to provide
  // appropriate manual mapping that makes sure the names match (the extras is
  // still a potential problem though -- we will only have them as
  // dependencies if we build against a native system package; maybe we can
  // add them manually with an option).
  //
  package_status system_package_manager_fedora::
  map_package (const package_name& pn,
               const version& pv,
               const available_packages& aps) const
  {
    // We should only have one available package corresponding to this package
    // name/version.
    //
    assert (aps.size () == 1);

    const shared_ptr<available_package>&        ap (aps.front ().first);
    const lazy_shared_ptr<repository_fragment>& rf (aps.front ().second);

    // Without explicit type, the best we can do in trying to detect whether
    // this is a library is to check for the lib prefix. Libraries without the
    // lib prefix and non-libraries with the lib prefix (both of which we do
    // not recomment) will have to provide a manual mapping (or explicit
    // type).
    //
    const string& pt (ap->effective_type ());

    strings ns (system_package_names (aps,
                                      os_release.name_id,
                                      os_release.version_id,
                                      os_release.like_ids,
                                      false /* native */));
    package_status r;
    if (ns.empty ())
    {
      // Automatically translate our package name similar to the consumption
      // case above. Except here we don't attempt to deduce main from -devel
      // or fallback to the project name, naturally.
      //
      const string& n (pn.string ());

      if (pt == "lib")
        r = package_status (n, n + "-devel");
      else
        r = package_status (n);
    }
    else
    {
      // Even though we only pass one available package, we may still end up
      // with multiple mappings. In this case we take the first, per the
      // documentation.
      //
      r = parse_name_value (pt,
                            ns.front (),
                            false /* need_doc */,
                            false /* need_debuginfo */,
                            false /* need_debugsource */);

      // If this is -devel without main, then derive main by stripping the
      // -devel suffix. This feels tighter than just using the bpkg package
      // name.
      //
      if (r.main.empty ())
      {
        assert (!r.devel.empty ());
        r.main.assign (r.devel, 0, r.devel.size () - 6);
      }
    }

    // Map the version.
    //
    // To recap, a Fedora package version has the following form:
    //
    // [<epoch>:]<version>-<release>
    //
    // Where <release> has the following form:
    //
    // <release-number>[.<distribution-tag>]
    //
    // For details on the ordering semantics, see the Fedora Versioning
    // Guidelines. While overall unsurprising, the only notable exceptions are
    // `~`, which sorts before anything else and is commonly used for upstream
    // pre-releases, and '^', which sorts after anything else and is
    // supposedly used for upstream post-release snapshots. For example,
    // 0.1.0~alpha.1-1.fc35 sorts earlier than 0.1.0-1.fc35.
    //
    // Ok, so how do we map our version to that? To recap, the bpkg version
    // has the following form:
    //
    // [+<epoch>-]<upstream>[-<prerel>][+<revision>]
    //
    // Let's start with the case where neither distribution nor upstream
    // version is specified and we need to derive everything from the bpkg
    // version.
    //
    // <epoch>
    //
    //   On one hand, if we keep the epoch, it won't necessarily match
    //   Fedora's native package epoch. But on the other it will allow our
    //   binary packages from different epochs to co-exist. Seeing that this
    //   can be easily overridden with a custom distribution version, let's
    //   keep it.
    //
    //   Note that while the Fedora start/default epoch is 0, ours is 1 (we
    //   use the 0 epoch for stub packages). So we will need to shift this
    //   value range.
    //
    //
    // <upstream>[-<prerel>]
    //
    //   Our upstream version maps naturally to Fedora's <version>. That is,
    //   our upstream version format/semantics is a subset of Fedora's
    //   <version>.
    //
    //   If this is a pre-release, then we could fail (that is, don't allow
    //   pre-releases) but then we won't be able to test on pre-release
    //   packages, for example, to make sure the name mapping is correct.
    //   Plus sometimes it's useful to publish pre-releases. We could ignore
    //   it, but then such packages will be indistinguishable from each other
    //   and the final release, which is not ideal. On the other hand, Fedora
    //   has the mechanism (`~`) which is essentially meant for this, so let's
    //   use it. We will use <prerel> as is since its format is the same as
    //   <upstream> and thus should map naturally.
    //
    //
    // <revision>
    //
    //   Similar to epoch, our revision won't necessarily match Fedora's
    //   native package release number. But on the other hand it will allow us
    //   to establish a correspondence between source and binary packages.
    //   Plus, upgrades between binary package releases will be handled
    //   naturally. Also note that the revision is mandatory in Fedora.
    //   Seeing that we allow overriding the releases with a custom
    //   distribution version (see below), let's use it.
    //
    //   Note that the Fedora start release number is 1 and our revision is
    //   0. So we will need to shift this value range.
    //
    //   Another related question is whether we should do anything about the
    //   distribution tag (.fc35, .el8, etc). Given that the use of hardcoded
    //   distribution tags in RPM spec files is strongly discouraged we will
    //   just rely on the standard approach to include the appropriate tag
    //   (while allowing the user to redefine it with an option). Note that
    //   the distribution tag is normally specified for the Release and
    //   Requires directives using the %{?dist} macro expansion and can be
    //   left unspecified for the Requires directive. For example:
    //
    //   Name: curl
    //   Version: 7.87.0
    //   Release: 1%{?dist}
    //   Requires: libcurl%{?_isa} >= %{version}-%{release}
    //   %global libpsl_version 1.2.3
    //   Requires: libpsl%{?_isa} >= %{libpsl_version}
    //
    // The next case to consider is when we have the upstream version
    // (upstream-version manifest value). After some rumination it feels
    // correct to use it in place of the <epoch>-<upstream> components in the
    // above mapping (upstream version itself cannot have epoch). In other
    // words, we will add the pre-release and revision components from the
    // bpkg version. If this is not the desired semantics, then it can always
    // be overrided with the distribution version.
    //
    // Finally, we have the distribution version. The <epoch> and <version>
    // components are straightforward: they should be specified by the
    // distribution version as required. This leaves pre-release and
    // release. It feels like in most cases we would want these copied over
    // from the bpkg version automatically -- it's too tedious and error-
    // prone to maintain them manually. However, we want the user to have the
    // full override ability. So instead, if empty release is specified, as in
    // 1.2.3-, then we automatically add bpkg revision. Similarly, if empty
    // pre-release is specified, as in 1.2.3~, then we add bpkg pre-release.
    // To add both automatically, we would specify 1.2.3~- (other combinations
    // are 1.2.3~b.1- and 1.2.3~-1). If specified, the release must not
    // contain the distribution tag, since it is deduced automatically using
    // the %{?dist} macro expansion if required. Also, since the release
    // component is mandatory in Fedora, if it is omitted together with the
    // separating dash we will add the release 1 automatically.
    //
    // Note also that per the RPM spec file format documentation neither
    // version nor release components may contain `:` or `-`. Note that the
    // bpkg upstream version may not contain either.
    //
    string& sv (r.system_version);

    if (optional<string> ov = system_package_version (ap,
                                                      rf,
                                                      os_release.name_id,
                                                      os_release.version_id,
                                                      os_release.like_ids))
    {
      string& dv (*ov);
      size_t n (dv.size ());

      // Find the package release and upstream pre-release positions, if any.
      //
      size_t rp (dv.rfind ('-'));
      size_t pp (dv.rfind ('~', rp));

      // Copy over the [<epoch>:]<version> part.
      //
      sv.assign (dv, 0, pp < rp ? pp : rp);

      // Add pre-release copying over the bpkg version value if empty.
      //
      if (pp != string::npos)
      {
        if (size_t pn = (rp != string::npos ? rp : n) - (pp + 1))
        {
          sv.append (dv, pp, pn + 1);
        }
        else
        {
          if (pv.release)
          {
            assert (!pv.release->empty ()); // Cannot be earliest special.
            sv += '~';
            sv += *pv.release;
          }
        }
      }

      // Add release copying over the bpkg version revision if empty.
      //
      if (rp != string::npos)
      {
        if (size_t rn = n - (rp + 1))
        {
          sv.append (dv, rp, rn + 1);
        }
        else
        {
          sv += '-';
          sv += to_string (pv.revision ? *pv.revision + 1 : 1);
        }
      }
      else
        sv += "-1"; // Default to 1 since the release is mandatory.
    }
    else
    {
      if (ap->upstream_version)
      {
        const string& uv (*ap->upstream_version);

        // Make sure the upstream version doesn't contain ':' and '-'
        // characters since they are not allowed in the <version> component
        // (see the RPM spec file format documentation for details).
        //
        // Note that this verification is not exhaustive and here we only make
        // sure that these characters are only used to separate the version
        // components.
        //
        size_t p (uv.find (":-"));
        if (p != string::npos)
          fail << "'" << uv[p] << "' character in upstream-version manifest "
               << "value " << uv << " of package " << pn << ' '
               << ap->version <<
            info << "consider specifying explicit " << os_release.name_id
                 << " version mapping in " << pn << " package manifest";

        sv += uv;
      }
      else
      {
        // Add epoch unless maps to 0.
        //
        assert (pv.epoch != 0); // Cannot be a stub.
        if (pv.epoch != 1)
        {
          sv = to_string (pv.epoch - 1);
          sv += ':';
        }

        sv += pv.upstream;
      }

      // Add pre-release.
      //
      if (pv.release)
      {
        assert (!pv.release->empty ()); // Cannot be earliest special.
        sv += '~';
        sv += *pv.release;
      }

      // Add revision.
      //
      sv += '-';
      sv += to_string (pv.revision ? *pv.revision + 1 : 1);
    }

    return r;
  }

  // Evaluate the specified expressions expanding the contained macros by
  // executing `rpm --eval <expr1> --eval <expr2>...` and return the list of
  // the resulting lines read from the process stdout. Note that an expression
  // may potentially end up with multiple lines which the caller is expected
  // to deal with (ensure fixed number of lines, eval only one expression,
  // etc).
  //
  strings system_package_manager_fedora::
  rpm_eval (const cstrings& opts, const cstrings& expressions)
  {
    strings r;

    if (expressions.empty ())
      return r;

    cstrings args;
    args.reserve (2 + opts.size () + expressions.size () * 2);

    args.push_back ("rpm");

    for (const char* o: opts)
      args.push_back (o);

    for (const char* e: expressions)
    {
      args.push_back ("--eval");
      args.push_back (e);
    }

    args.push_back (nullptr);

    try
    {
      process_path pp (process::path_search (args[0]));
      process_env pe (pp);

      if (verb >= 3)
        print_process (pe, args);

      process pr (pp, args, -2 /* stdin */, -1 /* stdout */, 2);

      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

        // The number of lines is normally equal to or greater than the number
        // of expressions.
        //
        r.reserve (expressions.size ());

        for (string l; !eof (getline (is, l)); )
          r.push_back (move (l));

        is.close ();
      }
      catch (const io_error& e)
      {
        if (pr.wait ())
          fail << "unable to read " << args[0] << " --eval output: " << e;

        // Fall through.
      }

      if (!pr.wait ())
      {
        diag_record dr (fail);
        dr << args[0] << " exited with non-zero code";

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

  // Some background on creating Fedora packages (for a bit more detailed
  // overview see the RPM Packaging Guide).
  //
  // An RPM package consists of the cpio archive, which contains the package
  // files plus the RPM header file with metadata about the package. The RPM
  // package manager uses this metadata to determine dependencies, where to
  // install files, and other information. There are two types of RPM
  // packages: source RPM and binary RPM. A source RPM contains source code,
  // optionally patches to apply, and the spec file, which describes how to
  // build the source code into a binary RPM. A binary RPM contains the
  // binaries built from the sources package. While it's possible to create
  // the package completely manually without using any of the Fedora tools, we
  // are not going to go this route (see reasons mentioned in the Debian
  // implementation for the list of issues with this approach).
  //
  // Based on this our plan is to produce an RPM spec file and then invoke
  // rpmbuild to produce the binary package from that. While this approach is
  // normally used to build things from source, it feels like we should be
  // able to pretend that we are. Specifially, we can implement the %install
  // section of the spec file to invoke the build system and install all the
  // packages directly from their bpkg locations.
  //
  // Note that the -debuginfo sub-packages are generated by default and all we
  // need to do from our side is to compile with debug information (-g),
  // failed which we get a warning from rpmbuild. We will also disable
  // generating the -debugsource sub-packages since that would require to set
  // up the source files infrastructure in the ~/rpmbuild/BUILD/ directory,
  // which feels too hairy for now.
  //
  // Note: this setup requires rpmdevtools (rpmdev-setuptree) and its
  // dependency rpm-build and rpm packages.
  //
  auto system_package_manager_fedora::
  generate (const packages& pkgs,
            const packages& deps,
            const strings& vars,
            const dir_path& cfg_dir,
            const package_manifest& pm,
            const string& pt,
            const small_vector<language, 1>& langs,
            optional<bool> recursive_full,
            bool /* first */) -> binary_files
  {
    tracer trace ("system_package_manager_fedora::generate");

    assert (!langs.empty ()); // Should be effective.

    const shared_ptr<selected_package>& sp (pkgs.front ().selected);
    const package_name& pn (sp->name);
    const version& pv (sp->version);

    // Use version without iteration in paths, etc.
    //
    string pvs (pv.string (false /* ignore_revision */,
                           true  /* ignore_iteration */));

    const available_packages& aps (pkgs.front ().available);

    bool lib (pt == "lib");
    bool priv (ops_->private_ ()); // Private installation.

    // For now we only know how to handle libraries with C-common interface
    // languages. But we allow other implementation languages.
    //
    if (lib)
    {
      for (const language& l: langs)
        if (!l.impl && l.name != "c" && l.name != "c++" && l.name != "cc")
          fail << l.name << " libraries are not yet supported";
    }

    // Return true if this package uses the specified language, only as
    // interface language if intf_only is true.
    //
    auto lang = [&langs] (const char* n, bool intf_only = false) -> bool
    {
      return find_if (langs.begin (), langs.end (),
                      [n, intf_only] (const language& l)
                      {
                        return (!intf_only || !l.impl) && l.name == n;
                      }) != langs.end ();
    };

    // As a first step, figure out the system names and version of the package
    // we are generating and all the dependencies, diagnosing anything fishy.
    // If the main package is not present for a dependency, then set the main
    // package name to an empty string.
    //
    // Note that there should be no duplicate dependencies and we can sidestep
    // the status cache.
    //
    package_status st (map_package (pn, pv, aps));

    vector<package_status> sdeps;
    sdeps.reserve (deps.size ());
    for (const package& p: deps)
    {
      const shared_ptr<selected_package>& sp (p.selected);
      const available_packages& aps (p.available);

      package_status s;
      if (sp->substate == package_substate::system)
      {
        // Note that for a system dependency the main package name is already
        // empty if it is not present in the distribution.
        //
        optional<package_status> os (status (sp->name, aps));

        if (!os || os->status != package_status::installed)
          fail << os_release.name_id << " package for " << sp->name
               << " system package is no longer installed";

        // For good measure verify the mapped back version still matches
        // configured. Note that besides the normal case (queried by the
        // system package manager), it could have also been specified by the
        // user as an actual version or a wildcard. Ignoring this check for a
        // wildcard feels consistent with the overall semantics.
        //
        if (sp->version != wildcard_version && sp->version != os->version)
        {
          fail << "current " << os_release.name_id << " package version for "
               << sp->name << " system package does not match configured" <<
            info << "configured version: " << sp->version <<
            info << "current version: " << os->version << " ("
               << os->system_version << ')';
        }

        s = move (*os);

        // Note that the system version retrieved with status() likely
        // contains the distribution tag in its release component. We,
        // however, don't want it to ever be mentioned in the spec file and so
        // just strip it right away. This will also make it consistent with
        // the non-system dependencies.
        //
        string& v (s.system_version);
        size_t p (v.find_last_of ("-."));
        assert (p != string::npos); // The release is mandatory.

        if (v[p] == '.')
          v.resize (p);
      }
      else
      {
        s = map_package (sp->name, sp->version, aps);

        // Set the main package name to an empty string if we wouldn't be
        // generating the main package for this dependency (binless library
        // without the -common sub-package).
        //
        assert (aps.size () == 1);

        const optional<string>& t (aps.front ().first->type);

        if (s.common.empty () &&
            package_manifest::effective_type (t, sp->name) == "lib")
        {
          strings sos (package_manifest::effective_type_sub_options (t));

          if (find (sos.begin (), sos.end (), "binless") != sos.end ())
            s.main.clear ();
        }
      }

      sdeps.push_back (move (s));
    }

    // We only allow the standard -debug* sub-package names.
    //
    if (!st.debuginfo.empty () && st.debuginfo != st.main + "-debuginfo")
      fail << "generation of -debuginfo packages with custom names not "
           << "supported" <<
        info << "use " << st.main << "-debuginfo name instead";

    if (!st.debugsource.empty () && st.debuginfo != st.main + "-debugsource")
      fail << "generation of -debugsource packages with custom names not "
           << "supported" <<
        info << "use " << st.main << "-debugsource name instead";

    // Prepare the common extra options that need to be passed to both
    // rpmbuild and rpm.
    //
    strings common_opts {"--target", arch};

    // Add the dist macro (un)definition if --fedora-dist-tag is specified.
    //
    if (ops_->fedora_dist_tag_specified ())
    {
      string dist (ops_->fedora_dist_tag ());

      if (!dist.empty ())
      {
        bool f (dist.front () == '+');
        bool b (dist.back () == '+');

        if (f && b) // Note: covers just `+`.
          fail << "invalid distribution tag '" << dist << "'";

        // If the distribution tag is specified with a leading/trailing '+',
        // then we query the default tag value and modify it using the
        // specified suffix/prefix.
        //
        // Note that we rely on the fact that the dist tag doesn't depend on
        // the --target option which we also pass to rpmbuild.
        //
        if (f || b)
        {
          string affix (move (dist));
          strings expansions (rpm_eval (cstrings (), cstrings {"%{?dist}"}));

          if (expansions.size () != 1)
            fail << "one line expected as an expansion of macro %{?dist}";

          dist = move (expansions[0]);

          // Normally, the default distribution tag starts with the dot, in
          // which case we insert the prefix after it. Note, however, that the
          // tag can potentially be re/un-defined (for example in
          // ~/.rpmmacros), so we need to also handle the potential absence of
          // the leading dot inserting the prefix right at the beginning in
          // this case.
          //
          if (f)
            dist.append (affix, 1, affix.size () - 1);
          else
            dist.insert (dist[0] == '.' ? 1 : 0, affix, 0, affix.size () - 1);
        }
        else
        {
          // Insert the leading dot into the distribution tag if missing.
          //
          if (dist.front () != '.')
            dist.insert (dist.begin (), '.');
        }

        common_opts.push_back ("--define=dist " + dist);
      }
      else
        common_opts.push_back ("--define=dist %{nil}");
    }

    // Evaluate the specified expressions expanding the contained macros. Make
    // sure these macros are expanded to the same values as if used in the
    // being generated spec file.
    //
    // Note that %{_docdir} and %{_licensedir} macros are set internally by
    // rpmbuild (may depend on DocDir spec file directive, etc which we will
    // not use) and thus cannot be queried with `rpm --eval` out of the
    // box. To allow using these macros in the expressions, we provide their
    // definitions to their default values on the command line.
    //
    auto eval = [&common_opts, this] (const cstrings& expressions)
    {
      cstrings opts;
      opts.reserve (common_opts.size () +
                    2                   +
                    ops_->fedora_query_option ().size ());

      // Pass the rpmbuild/rpm common options.
      //
      for (const string& o: common_opts)
        opts.push_back (o.c_str ());

      // Pass the %{_docdir} and %{_licensedir} macro definitions.
      //
      opts.push_back ("--define=_docdir %{_defaultdocdir}");
      opts.push_back ("--define=_licensedir %{_defaultlicensedir}");

      // Pass any additional options specified by the user.
      //
      for (const string& o: ops_->fedora_query_option ())
        opts.push_back (o.c_str ());

      return rpm_eval (opts, expressions);
    };

    // We override every config.install.* variable in order not to pick
    // anything configured. Note that we add some more in the spec file below.
    //
    // We make use of the <project> substitution since in the recursive mode
    // we may be installing multiple projects. Note that the <private>
    // directory component is automatically removed if this functionality is
    // not enabled. One side-effect of using <project> is that we will be
    // using the bpkg package name instead of the Fedora package name. But
    // perhaps that's correct: while in Fedora the source package name (which
    // is the same as the main binary package name) does not necessarily
    // correspond to the "logical" package name, we still want to use the
    // logical name (consider libsqlite3 which is mapped to sqlite-libs and
    // sqlite-devel; we don't want <project> to be sqlite-libs). To keep
    // things consistent we use the bpkg package name for <private> as well.
    //
    // Let's only use those directory macros which we can query with `rpm
    // --eval` (see eval() lambda for details). Note that this means our
    // installed_entries paths (see below) may not correspond exactly to where
    // things will actually be installed during rpmbuild. But that shouldn't
    // be an issue since we make sure to never use these paths directly in the
    // spec file (always using macros instead).
    //
    // NOTE: make sure to update the expressions evaluation and the %files
    //       sections below if changing anything here.
    //
    strings config {
      "config.install.root=%{_prefix}/",
      "config.install.data_root=%{_exec_prefix}/",
      "config.install.exec_root=%{_exec_prefix}/",

      "config.install.bin=%{_bindir}/",
      "config.install.sbin=%{_sbindir}/",

      // On Fedora shared libraries should be executable.
      //
      "config.install.lib=%{_libdir}/<private>/",
      "config.install.lib.mode=755",
      "config.install.libexec=%{_libexecdir}/<private>/<project>/",
      "config.install.pkgconfig=lib/pkgconfig/",

      "config.install.etc=%{_sysconfdir}/",
      "config.install.include=%{_includedir}/<private>/",
      "config.install.include_arch=include/",
      "config.install.share=%{_datadir}/",
      "config.install.data=share/<private>/<project>/",
      "config.install.buildfile=share/build2/export/<project>/",

      "config.install.doc=%{_docdir}/<private>/<project>/",
      "config.install.legal=%{_licensedir}/<private>/<project>/",
      "config.install.man=%{_mandir}/",
      "config.install.man1=man/man1/",
      "config.install.man2=man/man2/",
      "config.install.man3=man/man3/",
      "config.install.man4=man/man4/",
      "config.install.man5=man/man5/",
      "config.install.man6=man/man6/",
      "config.install.man7=man/man7/",
      "config.install.man8=man/man8/"};

    config.push_back ("config.install.private=" +
                      (priv ? pn.string () : "[null]"));

    // Add user-specified configuration variables last to allow them to
    // override anything.
    //
    for (const string& v: vars)
      config.push_back (v);

    // Note that we need to expand macros in the configuration variables
    // before passing them to the below installed_entries() call.
    //
    // Also note that we expand the variables passed on the command line as
    // well. While this can be useful, it can also be surprising. However, it
    // is always possible to escape the '%' character which introduces the
    // macro expansion, which in most cases won't be necessary since an
    // undefined macro expansion is preserved literally.
    //
    // While at it, also obtain some other information that we will need down
    // the road.
    //
    strings  expansions;

    // Installed entry directories for sorting out the installed files into
    // the %files sections of the sub-packages.
    //
    // We put exported buildfiles into the main package, which makes sense
    // after some meditation: they normally contain rules and are bundled
    // either with a tool (say, thrift), a module (say, libbuild2-thrift), or
    // an add-on package (say, thrift-build2).
    //
    dir_path bindir;
    dir_path sbindir;
    dir_path libexecdir;
    dir_path confdir;
    dir_path incdir;
    dir_path bfdir;
    dir_path libdir;
    dir_path pkgdir;     // Not queried, set as libdir/pkgconfig/.
    dir_path sharedir;
    dir_path docdir;
    dir_path mandir;
    dir_path licensedir;
    dir_path build2dir;

    // Note that the ~/rpmbuild/{.,BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
    // directory paths used by rpmbuild are actually defined as the
    // %{_topdir}, %{_builddir}, %{_buildrootdir}, %{_rpmdir}, %{_sourcedir},
    // %{_specdir}, and %{_srcrpmdir} RPM macros. These macros can potentially
    // be redefined in RPM configuration files, in particular, in
    // ~/.rpmmacros.
    //
    dir_path topdir;  // ~/rpmbuild/
    dir_path specdir; // ~/rpmbuild/SPECS/

    // RPM file absolute path template.
    //
    // Note that %{_rpmfilename} normally expands as the following template:
    //
    // %{ARCH}/%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}.rpm
    //
    string rpmfile;
    {
      cstrings expressions;
      expressions.reserve (config.size () + 13);

      for (const string& c: config)
        expressions.push_back (c.c_str ());

      expressions.push_back ("%{?_bindir}");
      expressions.push_back ("%{?_sbindir}");
      expressions.push_back ("%{?_libexecdir}");
      expressions.push_back ("%{?_sysconfdir}");
      expressions.push_back ("%{?_includedir}");
      expressions.push_back ("%{?_libdir}");
      expressions.push_back ("%{?_datadir}");
      expressions.push_back ("%{?_docdir}");
      expressions.push_back ("%{?_mandir}");
      expressions.push_back ("%{?_licensedir}");

      expressions.push_back ("%{?_topdir}");
      expressions.push_back ("%{?_specdir}");

      expressions.push_back ("%{_rpmdir}/%{_rpmfilename}");

      // Note that we rely on the fact that these macros are defined while
      // refer to them in the spec file, etc. Thus, let's verify that and fail
      // early if that's not the case for whatever reason.
      //
      expressions.push_back ("%{?_rpmdir}");
      expressions.push_back ("%{?_rpmfilename}");
      expressions.push_back ("%{?_usrsrc}");
      expressions.push_back ("%{?buildroot}");

      // Note that if the architecture passed with the --target option is
      // invalid, then rpmbuild will fail with some ugly diagnostics since
      // %{_arch} macro stays unexpanded in some commands executed by
      // rpmbuild. Thus, let's verify that the architecture is recognized by
      // rpmbuild and fail early if that's not the case.
      //
      expressions.push_back ("%{?_arch}");

      expansions = eval (expressions);

      // Shouldn't happen unless some paths contain newlines, which we don't
      // care about.
      //
      if (expansions.size () != expressions.size ())
        fail << "number of RPM directory path expansions differs from number "
             << "of path expressions";

      // Pop the string/directory expansions.
      //
      auto pop_string = [&expansions, &expressions] ()
      {
        assert (!expansions.empty ());

        string r (move (expansions.back ()));

        if (r.empty ())
          fail << "macro '" << expressions.back () << "' expands into empty "
               << "string";

        expansions.pop_back ();
        expressions.pop_back ();
        return r;
      };

      auto pop_path = [&expansions, &expressions] ()
      {
        assert (!expansions.empty ());

        try
        {
          path r (move (expansions.back ()));

          if (r.empty ())
            fail << "macro '" << expressions.back () << "' expands into empty "
                 << "path";

          expansions.pop_back ();
          expressions.pop_back ();
          return r;
        }
        catch (const invalid_path& e)
        {
          fail << "macro '" << expressions.back () << "' expands into invalid "
               << "path '" << e.path << "'" << endf;
        }
      };

      auto pop_dir = [&pop_path] ()
      {
        return path_cast<dir_path> (pop_path ());
      };

      // The source of a potentially invalid architecture is likely to be the
      // --architecture option specified by the user. But can probably also be
      // some mis-configuration.
      //
      if (expansions.back ().empty ()) // %{?_arch}
        fail << "unknown target architecture '" << arch << "'";

      // We only need the following macro expansions for the verification.
      //
      pop_string (); // %{?_arch}
      pop_dir ();    // %{?buildroot}
      pop_dir ();    // %{?_usrsrc}
      pop_string (); // %{?_rpmfilename}
      pop_dir ();    // %{?_rpmdir}

      rpmfile = pop_string ();
      specdir = pop_dir ();
      topdir  = pop_dir ();

      // Let's tighten things up and only look for the installed files in
      // <private>/ (if specified) to make sure there is nothing stray.
      //
      dir_path pd (priv ? pn.string () : "");

      licensedir = pop_dir () / pd;
      mandir     = pop_dir ();
      docdir     = pop_dir () / pd;
      sharedir   = pop_dir ();
      build2dir  = sharedir / dir_path ("build2");
      bfdir      = build2dir / dir_path ("export");
      sharedir  /= pd;
      libdir     = pop_dir () / pd;
      pkgdir     = libdir / dir_path ("pkgconfig");
      incdir     = pop_dir () / pd;
      confdir    = pop_dir ();
      libexecdir = pop_dir () / pd;
      sbindir    = pop_dir ();
      bindir     = pop_dir ();

      // Only configuration variables expansions must remain.
      //
      assert (expansions.size () == config.size ());
    }

    // Note that the conventional place for all the inputs and outputs of the
    // rpmbuild operations is the directory tree rooted at ~/rpmbuild/. We
    // won't fight with rpmbuild and will use this tree as the user would
    // do while creating the binary package manually.
    //
    // Specifially, we will create the RPM spec file in ~/rpmbuild/SPECS/,
    // install the package(s) under the ~/rpmbuild/BUILDROOT/<package-dir>/
    // chroot, and expect the generated RPM files under ~/rpmbuild/RPMS/.
    //
    // That, in particular, means that we have no use for the --output-root
    // directory. We will also make sure that we don't overwrite an existing
    // RPM spec file unless --wipe-output is specified.
    //
    if (ops_->output_root_specified () && ops_->output_root () != topdir)
      fail << "--output-root|-o must be " << topdir << " if specified";

    // Note that in Fedora the Name spec file directive names the source
    // package as well as the main binary package and the spec file should
    // match this name.
    //
    // @@ TODO (maybe/later): it's unclear whether it's possible to rename
    //    the main binary package. Maybe makes sense to investigate if/when
    //    we decide to generate source packages.
    //
    path spec (specdir / (st.main + ".spec"));

    if (exists (spec) && !ops_->wipe_output ())
      fail << "RPM spec file " << spec << " already exists" <<
        info << "use --wipe-output to remove but be careful";

    // Note that we can use weak install scope for the auto recursive mode
    // since we know dependencies cannot be spread over multiple linked
    // configurations.
    //
    string scope (!recursive_full || *recursive_full ? "project" : "weak");

    // Get the map of files that will end up in the binary packages.
    //
    installed_entry_map ies (
      installed_entries (*ops_, pkgs, expansions, scope));

    if (ies.empty ())
      fail << "specified package(s) do not install any files";

    if (verb >= 4)
    {
      for (const auto& p: ies)
      {
        diag_record dr (trace);
        dr << "installed entry: " << p.first;

        if (p.second.target != nullptr)
          dr << " -> " << p.second.target->first; // Symlink.
        else
          dr << ' ' << p.second.mode;
      }
    }

    // As an optimization, don't generate the main and -debug* packages for a
    // binless library unless it also specifies the -common sub-package.
    //
    // If this is a binless library, then verify that it doesn't install any
    // executable, library, or configuration files. Also verify that it has
    // the -devel sub-package but doesn't specify the -static sub-package.
    //
    bool binless (false);

    if (lib)
    {
      assert (aps.size () == 1);

      const shared_ptr<available_package>& ap (aps.front ().first);
      strings sos (package_manifest::effective_type_sub_options (ap->type));

      if (find (sos.begin (), sos.end (), "binless") != sos.end ())
      {
        // Verify installed files.
        //
        auto bad_install = [&pn, &pv] (const string& w)
        {
          fail << "binless library " << pn << ' ' << pv << " installs " << w;
        };

        auto verify_not_installed = [&ies, &bad_install] (const dir_path& d)
        {
          auto p (ies.find_sub (d));
          if (p.first != p.second)
            bad_install (p.first->first.string ());
        };

        verify_not_installed (bindir);
        verify_not_installed (sbindir);
        verify_not_installed (libexecdir);

        // It would probably be better not to fail here but generate the main
        // package instead (as we do if the -common sub-package is also being
        // generated). Then, however, it would not be easy to detect if a
        // dependency has the main package or not (see sdeps initialization
        // for details).
        //
        verify_not_installed (confdir);

        for (auto p (ies.find_sub (libdir)); p.first != p.second; ++p.first)
        {
          const path& f (p.first->first);

          if (!f.sub (pkgdir))
            bad_install (f.string ());
        }

        // Verify sub-packages.
        //
        if (st.devel.empty ())
          fail << "binless library " << pn << ' ' << pv << " doesn't have "
               << os_release.name_id << " -devel package";

        if (!st.static_.empty ())
          fail << "binless library " << pn << ' ' << pv << " has "
               << os_release.name_id << ' ' << st.static_ << " package";

        binless = true;
      }
    }

    bool gen_main (!binless || !st.common.empty ());

    // If we don't generate the main package (and thus the -common
    // sub-package), then fail if there are any data files installed. It would
    // probably be better not to fail but generate the main package instead in
    // this case. Then, however, it would not be easy to detect if a
    // dependency has the main package or not.
    //
    if (!gen_main)
    {
      for (auto p (ies.find_sub (sharedir)); p.first != p.second; ++p.first)
      {
        const path& f (p.first->first);

        if (!f.sub (docdir) && !f.sub (mandir) && !f.sub (licensedir))
        {
          fail << "binless library " << pn << ' ' << pv << " installs " << f <<
            info << "consider specifying -common package in explicit "
                 << os_release.name_id << " name mapping in package manifest";
        }
      }

      for (auto p (ies.find_sub (bfdir)); p.first != p.second; ++p.first)
      {
        const path& f (p.first->first);

        fail << "binless library " << pn << ' ' << pv << " installs " << f <<
            info << "consider specifying -common package in explicit "
             << os_release.name_id << " name mapping in package manifest";
      }
    }

    if (verb >= 3)
    {
      auto print_status = [] (diag_record& dr,
                              const package_status& s,
                              const string& main)
      {
        dr << (main.empty () ? "" : " ") << main
           << (s.devel.empty () ? "" : " ") << s.devel
           << (s.static_.empty () ? "" : " ") << s.static_
           << (s.doc.empty () ? "" : " ") << s.doc
           << (s.debuginfo.empty () ? "" : " ") << s.debuginfo
           << (s.debugsource.empty () ? "" : " ") << s.debugsource
           << (s.common.empty () ? "" : " ") << s.common
           << ' ' << s.system_version;
      };

      {
        diag_record dr (trace);
        dr << "package:";
        print_status (dr, st, gen_main ? st.main : empty_string);
      }

      for (const package_status& s: sdeps)
      {
        diag_record dr (trace);
        dr << "dependency:";
        print_status (dr, s, s.main);
      }
    }

    // Prepare the data for the RPM spec file.
    //
    // Url directive.
    //
    string url (pm.package_url ? pm.package_url->string () :
                pm.url         ? pm.url->string ()         :
                string ());

    // Packager directive.
    //
    string packager;
    if (ops_->fedora_packager_specified ())
    {
      packager = ops_->fedora_packager ();
    }
    else
    {
      const email* e (pm.package_email ? &*pm.package_email :
                      pm.email         ? &*pm.email         :
                      nullptr);

      if (e == nullptr)
        fail << "unable to determine packager from manifest" <<
          info << "specify explicitly with --fedora-packager";

      // In certain places (e.g., %changelog), Fedora expect this to be in the
      // `John Doe <john@example.org>` form while we often specify just the
      // email address (e.g., to the mailing list). Try to detect such a case
      // and complete it to the desired format.
      //
      if (e->find (' ') == string::npos && e->find ('@') != string::npos)
      {
        // Try to use comment as name, if any.
        //
        if (!e->comment.empty ())
        {
          packager = e->comment;

          // Strip the potential trailing dot.
          //
          if (packager.back () == '.')
            packager.pop_back ();
        }
        else
          packager = pn.string () + " package maintainer";

        packager += " <" + *e + '>';
      }
      else
        packager = *e;
    }

    // Version, Release, and Epoch directives.
    //
    struct system_version
    {
      string epoch;
      string version;
      string release;
    };

    auto parse_system_version = [] (const string& v)
    {
      system_version r;

      size_t e (v.find (':'));
      if (e != string::npos)
        r.epoch = string (v, 0, e);

      size_t b (e != string::npos ? e + 1 : 0);
      e = v.find ('-', b);
      assert (e != string::npos); // Release is required.

      r.version = string (v, b, e - b);

      b = e + 1;
      r.release = string (v, b);
      return r;
    };

    system_version sys_version (parse_system_version (st.system_version));

    // License directive.
    //
    // The directive value is a SPDX license expression. Note that the OR/AND
    // operators must be specified in upper case and the AND operator has a
    // higher precedence than OR.
    //
    string license;
    for (const licenses& ls: pm.license_alternatives)
    {
      if (!license.empty ())
        license += " OR ";

      for (auto b (ls.begin ()), i (b); i != ls.end (); ++i)
      {
        if (i != b)
          license += " AND ";

        license += *i;
      }
    }

    // Create the ~/rpmbuild directory tree if it doesn't exist yet.
    //
    if (!exists (topdir))
    {
      cstrings args {"rpmdev-setuptree", nullptr};

      try
      {
        process_path pp (process::path_search (args[0]));
        process_env pe (pp);

        if (verb >= 3)
          print_process (pe, args);

        process pr (pp, args);

        if (!pr.wait ())
        {
          diag_record dr (fail);
          dr << args[0] << " exited with non-zero code";

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

      // For good measure verify that ~/rpmbuild directory now exists.
      //
      if (!exists (topdir))
        fail << "unable to create RPM build directory " << topdir;
    }

    // We cannot easily detect architecture-independent packages (think
    // libbutl.bash) and providing an option feels like the best we can do.
    // Note that the noarch value means architecture-independent and any other
    // value means architecture-dependent.
    //
    const string& build_arch (ops_->fedora_build_arch_specified ()
                              ? ops_->fedora_build_arch ()
                              : empty_string);

    // The RPM spec file.
    //
    // Note that we try to do a reasonably thorough job (e.g., using macros
    // rather than hardcoding values, trying to comply with Fedora guidelines
    // and recommendations, etc) with the view that this can be used as a
    // starting point for manual packaging.
    //
    // NOTE: if changing anything here make sure that all the macros expanded
    //       in the spec file unconditionally are defined (see above how we do
    //       that for the _usrsrc macro as an example).
    //
    try
    {
      ofdstream os (spec);

      // Note that Fedora Packaging Guidelines recommend to declare the
      // package dependencies in the architecture-specific fashion using the
      // %{?_isa} macro in the corresponding Requires directive (e.g.,
      // `Requires: foo%{?_isa}` which would expand to something like
      // `Requires: foo(x86-64)`). We, however, cannot easily detect if the
      // distribution packages which correspond to the bpkg package
      // dependencies are architecture-specific or not. Thus, we will generate
      // the architecture-independent Requires directives for them which
      // postpones the architecture resolution until the package installation
      // time by dnf. We could potentially still try to guess if the
      // dependency package is architecture-specific or not based on its
      // languages, but let's keep it simple for now seeing that it's not a
      // deal breaker.
      //
      // Also note that we will generate the architecture-specific
      // dependencies on our own sub-packages, unless the --fedora-build-arch
      // option has been specified, and for the C/C++ language related
      // dependencies (glibc, etc). In other words, we will not try to craft
      // the architecture specifier ourselves when we cannot use %{?_isa}.
      //
      string isa (build_arch.empty () ? "%{?_isa}" : "");

      // Add the Requires directive(s), optionally separating them from the
      // previous directives with an empty line.
      //
      auto add_requires = [&os] (bool& first, const string& v)
      {
        if (first)
        {
          os << '\n';
          first = false;
        }

        os << "Requires: " << v << '\n';
      };

      auto add_requires_list = [&add_requires] (bool& first, const strings& vs)
      {
        for (const string& v: vs)
          add_requires (first, v);
      };

      // Add the Requires directives for language dependencies of a
      // sub-package. Deduce the language dependency packages (such as glibc,
      // libstdc++, etc), unless they are specified explicitly via the
      // --fedora-*-langreq options. If single option with an empty value is
      // specified, then no language dependencies are added. The valid
      // sub-package suffixes are '' (main package), '-devel', and '-static'.
      //
      auto add_lang_requires = [&lang, &add_requires, &add_requires_list]
                               (bool& first,
                                const string& suffix,
                                const strings& options,
                                bool intf_only = false)
      {
        if (!options.empty ())
        {
          if (options.size () != 1 || !options[0].empty ())
            add_requires_list (first, options);
        }
        else
        {
          // Add dependency on libstdc++<suffix> and glibc<suffix> packages.
          //
          // It doesn't seems that the -static sub-package needs to define any
          // default C/C++ language dependencies. That is a choice of the
          // dependent packages which may want to link the standard libraries
          // either statically or dynamically, so let's leave if for them to
          // arrange.
          //
          if (suffix != "-static")
          {
            // If this is an undetermined C-common library, we assume it may
            // be C++ (better to over- than under-specify).
            //
            bool cc (lang ("cc", intf_only));
            if (cc || lang ("c++", intf_only))
              add_requires (first, string ("libstdc++") + suffix + "%{?_isa}");

            if (cc || lang ("c", intf_only))
              add_requires (first, string ("glibc") + suffix + "%{?_isa}");
          }
        }
      };

      // We need to add the mandatory Summary and %description directives both
      // for the main package and for the sub-packages. In the Summary
      // directives we will use the `summary` package manifest value. In the
      // %description directives we will just describe the sub-package content
      // since using the `description` package manifest value is not going to
      // be easy: it can be arbitrarily long and may not even be plain text
      // (it's commonly the contents of the README.md file).
      //
      // We will disable automatic dependency discovery for all sub-packages
      // using the `AutoReqProv: no` directive since we have an accurate set
      // and some of them may not be system packages.
      //

      // The common information and the main package.
      //
      {
        os <<   "Name: " << st.main                                     << '\n'
           <<   "Version: " << sys_version.version                      << '\n'
           <<   "Release: " << sys_version.release << "%{?dist}"        << '\n';

        if (!sys_version.epoch.empty ())
          os << "Epoch: " << sys_version.epoch                          << '\n';

        os <<   "License: " << license                                  << '\n'
           <<   "Summary: " << pm.summary                               << '\n'
           <<   "Url: " << url                                          << '\n';

        if (!packager.empty ())
          os << "Packager: " << packager                                << '\n';

#if 0
        os <<   "#Source: https://pkg.cppget.org/1/???/"
           <<     pm.effective_project () << '/' << sp->name << '-'
           <<     sp->version << ".tar.gz" << '\n';
#endif

        // Idiomatic epoch-version-release value.
        //
        os <<   '\n'
           <<   "%global evr %{?epoch:%{epoch}:}%{version}-%{release}"  << '\n';

        if (gen_main)
        {
          os << '\n'
             << "# " << st.main                                         << '\n'
             << "#"                                                     << '\n';

          if (!build_arch.empty ())
            os << "BuildArch: " << build_arch                           << '\n';

          os << "AutoReqProv: no"                                       << '\n';

          // Requires directives.
          //
          {
            bool first (true);
            if (!st.common.empty ())
              add_requires (first, st.common + " = %{evr}");

            for (const package_status& s: sdeps)
            {
              if (!s.main.empty ())
                add_requires (first, s.main + " >= " + s.system_version);
            }

            add_lang_requires (first,
                               "" /* suffix */,
                               ops_->fedora_main_langreq ());

            if (ops_->fedora_main_extrareq_specified ())
              add_requires_list (first, ops_->fedora_main_extrareq ());
          }
        }

        // Note that we need to add the %description directive regardless if
        // the main package is being generated or not.
        //
        if (!binless)
        {
          os << '\n'
             << "%description"                                          << '\n'
             << "This package contains the runtime files."              << '\n';
        }
        else
        {
          os << '\n'
             << "%description"                                          << '\n'
             << "This package contains the development files."          << '\n';
        }
      }

      // The -devel sub-package.
      //
      if (!st.devel.empty ())
      {
        os <<   '\n'
           <<   "# " << st.devel                                        << '\n'
           <<   "#"                                                     << '\n'
           <<   "%package -n " << st.devel                              << '\n'
           <<   "Summary: " << pm.summary                               << '\n';

        // Feels like the architecture should be the same as for the main
        // package.
        //
        if (!build_arch.empty ())
          os << "BuildArch: " << build_arch                             << '\n';

        os <<   '\n'
           <<   "AutoReqProv: no"                                       << '\n';

        // Requires directives.
        //
        {
          bool first (true);

          // Dependency on the main package.
          //
          if (gen_main)
            add_requires (first, "%{name}" + isa + " = %{evr}");

          for (const package_status& s: sdeps)
          {
            // Doesn't look like we can distinguish between interface and
            // implementation dependencies here. So better to over- than
            // under-specify.
            //
            // Note that if the -devel sub-package doesn't exist for a
            // dependency, then its potential content may be part of the main
            // package. If that's the case we, strictly speaking, should add
            // the dependency on the main package. Let's, however, skip that
            // since we already have this dependency implicitly via our own
            // main package, which the -devel sub-package depends on.
            //
            if (!s.devel.empty ())
              add_requires (first, s.devel + " >= " + s.system_version);
          }

          add_lang_requires (first,
                             "-devel",
                             ops_->fedora_devel_langreq (),
                             true /* intf_only */);

          if (ops_->fedora_devel_extrareq_specified ())
            add_requires_list (first, ops_->fedora_devel_extrareq ());
        }

        // If the -static sub-package is not being generated but there are
        // some static libraries installed, then they will be added to the
        // -devel sub-package. If that's the case, we add the
        // `Provides: %{name}-static` directive for the -devel sub-package, as
        // recommended.
        //
        // Should we do the same for the main package, where the static
        // libraries go if the -devel sub-package is not being generated
        // either? While it feels sensible, we've never seen such a practice
        // or recommendation. So let's not do it for now.
        //
        if (st.static_.empty ())
        {
          for (auto p (ies.find_sub (libdir)); p.first != p.second; ++p.first)
          {
            const path& f (p.first->first);
            path l (f.leaf (libdir));
            const string& n (l.string ());

            if (l.simple ()                                   &&
                n.size () > 3 && n.compare (0, 3, "lib") == 0 &&
                l.extension () == "a")
            {
              os << '\n'
                 << "Provides: %{name}-static" << isa << " = %{evr}" << '\n';

              break;
            }
          }
        }

        os <<   '\n'
           <<   "%description -n " << st.devel                          << '\n'
           <<   "This package contains the development files."          << '\n';
      }

      // The -static sub-package.
      //
      if (!st.static_.empty ())
      {
        os <<   '\n'
           <<   "# " << st.static_                                      << '\n'
           <<   "#"                                                     << '\n'
           <<   "%package -n " << st.static_                            << '\n'
           <<   "Summary: " << pm.summary                               << '\n';

        // Feels like the architecture should be the same as for the -devel
        // sub-package.
        //
        if (!build_arch.empty ())
          os << "BuildArch: " << build_arch                             << '\n';

        os <<   '\n'
           <<   "AutoReqProv: no"                                       << '\n';

        // Requires directives.
        //
        {
          bool first (true);

          // The static libraries without headers doesn't seem to be of any
          // use. Thus, add dependency on the -devel or main sub-package, if
          // not being generated.
          //
          // Note that if there is no -devel package, then this cannot be a
          // binless library and thus the main package is being generated.
          //
          add_requires (
            first,
            (!st.devel.empty () ? st.devel : "%{name}") + isa + " = %{evr}");

          // Add dependency on sub-packages that may contain static libraries.
          // Note that in the -devel case we can potentially over-specify the
          // dependency which is better than to under-specify.
          //
          for (const package_status& s: sdeps)
          {
            // Note that if the -static sub-package doesn't exist for a
            // dependency, then its potential content may be part of the
            // -devel sub-package, if exists, or the main package otherwise.
            // If that's the case we, strictly speaking, should add the
            // dependency on the -devel sub-package, if exists, or the main
            // package otherwise. Let's, however, also consider the implicit
            // dependencies via our own -devel and main (sub-)packages, which
            // we depend on, and simplify things similar to what we do for the
            // -devel sub-package above.
            //
            // Also note that we only refer to the dependency's -devel
            // sub-package if we don't have our own -devel sub-package
            // (unlikely, but possible), which would provide us with such an
            // implicit dependency.
            //
            const string& p (!s.static_.empty () ? s.static_ :
                             st.devel.empty ()   ? s.devel   :
                             empty_string);

            if (!p.empty ())
              add_requires (first, p + " >= " + st.system_version);
          }

          add_lang_requires (first, "-static", ops_->fedora_stat_langreq ());

          if (ops_->fedora_stat_extrareq_specified ())
            add_requires_list (first, ops_->fedora_stat_extrareq ());
        }

        os <<   '\n'
           <<   "%description -n " << st.static_                        << '\n'
           <<   "This package contains the static libraries."           << '\n';
      }

      // The -doc sub-package.
      //
      if (!st.doc.empty ())
      {
        os <<   '\n'
           <<   "# " << st.doc                                          << '\n'
           <<   "#"                                                     << '\n'
           <<   "%package -n " << st.doc                                << '\n'
           <<   "Summary: " << pm.summary                               << '\n'
           <<   "BuildArch: noarch"                                     << '\n'
           <<   '\n'
           <<   "AutoReqProv: no"                                       << '\n'
           <<   '\n'
           <<   "%description -n " << st.doc                            << '\n'
           <<   "This package contains the documentation."              << '\n';
      }

      // The -common sub-package.
      //
      if (!st.common.empty ())
      {
        // Generally, this sub-package is not necessarily architecture-
        // independent (for example, it could contain something shared between
        // multiple binary packages produced from the same source package
        // rather than something shared between all the architectures of a
        // binary package). But seeing that we always generate one binary
        // package, for us it only makes sense as architecture-independent.
        //
        // It's also not clear what dependencies we can deduce for this
        // sub-package. Assuming that it depends on all the dependency -common
        // sub-packages is probably unreasonable.
        //
        os <<   '\n'
           <<   "# " << st.common                                       << '\n'
           <<   "#"                                                     << '\n'
           <<   "%package -n " << st.common                             << '\n'
           <<   "Summary: " << pm.summary                               << '\n'
           <<   "BuildArch: noarch"                                     << '\n'
           <<   '\n'
           <<   "AutoReqProv: no"                                       << '\n'
           <<   '\n'
           <<   "%description -n " << st.common                         << '\n'
           <<   "This package contains the architecture-independent files." << '\n';
      }

      // Build setup.
      //
      {
        bool lang_c   (lang ("c"));
        bool lang_cxx (lang ("c++"));
        bool lang_cc  (lang ("cc"));

        os <<   '\n'
           <<   "# Build setup."                                        << '\n'
           <<   "#"                                                     << '\n';

        // The -debuginfo and -debugsource sub-packages.
        //
        // Note that the -debuginfo and -debugsource sub-packages are defined
        // in the spec file by expanding the %{debug_package} macro (search
        // the macro definition in `rpm --showrc` stdout for details). This
        // expansion happens as part of the %install section processing but
        // only if the %{buildsubdir} macro is defined. This macro refers to
        // the package source subdirectory in the ~/rpmbuild/BUILD directory
        // and is normally set by the %setup macro expansion in the %prep
        // section which, in particular, extracts source files from an
        // archive, defines the %{buildsubdir} macro, and make this directory
        // current. Since we don't have an archive to extract, we will use the
        // %setup macro disabling sources extraction (-T) and creating an
        // empty source directory instead (-c). This directory is also used by
        // rpmbuild for saving debuginfo-related intermediate files
        // (debugfiles.list, etc). See "Fedora Debuginfo packages" and "Using
        // RPM build flags" documentation for better understanding what's
        // going on under the hood. There is also the "[Rpm-ecosystem] Trying
        // to understand %buildsubdir and debuginfo generation" mailing list
        // thread which provides some additional clarifications.
        //
        // Also note that we disable generating the -debugsource sub-packages
        // (see the generate() description above for the reasoning).
        //
        // For a binless library no -debug* packages are supposed to be
        // generated. Thus, we just drop their definitions by redefining the
        // %{debug_package} macro as an empty string.
        //
        if (!binless)
        {
          os << "%undefine _debugsource_packages"                       << '\n';

          // Append the -ffile-prefix-map option which is normally used to
          // strip source file path prefix in debug information (besides other
          // places). By default it is not used since rpmbuild replaces the
          // recognized prefixes by using the debugedit program (see below for
          // details) and we cannot rely on that since in our case the prefix
          // (bpkg configuration directory) is not recognized. We need to
          // replace the bpkg configuration directory prefix in the source
          // file paths with the destination directory where the -debugsource
          // sub-package files would be installed (if we were to generate it).
          // For example:
          //
          // /usr/src/debug/foo-1.0.0-1.fc35.x86_64
          //
          // There is just one complication:
          //
          // While the generation of the -debugsource sub-packages is
          // currently disabled, the executed by rpmbuild find-debuginfo
          // script still performs some preparations for them. It runs the
          // debugedit program, which, in particular, reads the source file
          // paths from the debug information in package binaries and saves
          // those which start with the ~/rpmbuild/BUILD/foo-1.0.0 or
          // /usr/src/debug/foo-1.0.0-1.fc35.x86_64 directory prefix into the
          // ~/rpmbuild/BUILD/foo-1.0.0/debugsources.list file, stripping the
          // prefixes. It also saves all the relative source file paths as is
          // (presumably assuming they are sub-entries of the
          // ~/rpmbuild/BUILD/foo-1.0.0 directory where the package archive
          // would normally be extracted). Afterwards, the content of the
          // debugsources.list file is piped as an input to the cpio program
          // executed in the ~/rpmbuild/BUILD/foo-1.0.0 directory as its
          // current working directory, which tries to copy these source files
          // to the
          // ~/rpmbuild/BUILDROOT/foo-1.0.0-1.fc35.x86_64/usr/src/debug/foo-1.0.0-1.fc35.x86_64
          // directory. Given that these source files are actually located in
          // the bpkg configuration directory rather than in the
          // ~/rpmbuild/BUILD/foo-1.0.0 directory the cpio program fails to
          // stat them and complains. To work around that we need to change
          // the replacement directory path in the -ffile-prefix-map option
          // value with some other absolute (not necessarily existing) path
          // which is not a subdirectory of the directory prefixes recognized
          // by the debugedit program. This way debugedit won't recognize any
          // of the package source files, will create an empty
          // debugsources.list file, and thus the cpio program won't try to
          // copy anything. Not to confuse the user (who can potentially see
          // such paths in gdb while examining a core file produced by the
          // package binary), we will keep this replacement directory path
          // close to the desired one, but will also make it clear that the
          // path is bogus:
          //
          // /usr/src/debug/bogus/foo-1.0.0-1.fc35.x86_64
          //
          // Note that this path mapping won't work for external packages with
          // source out of configuration (e.g., managed by bdep).
          //
          // @@ Supposedly this code won't be necessary when we add support
          //    for -debugsource sub-packages somehow. Feels like one way
          //    would be to make ~/rpmbuild/BUILD/foo-1.0.0 a symlink to the
          //    bpkg configuration (or to the primary package inside, if not
          //    --recursive).
          //
          if (ops_->fedora_buildflags () != "ignore")
          {
            const char* debugsource_dir (
              "%{_usrsrc}/debug/bogus/%{name}-%{evr}.%{_arch}");

            if (lang_c || lang_cc)
              os << "%global build_cflags %{?build_cflags} -ffile-prefix-map="
                 << cfg_dir.string () << '=' << debugsource_dir << '\n';

            if (lang_cxx || lang_cc)
              os << "%global build_cxxflags %{?build_cxxflags} -ffile-prefix-map="
                 << cfg_dir.string () << '=' << debugsource_dir << '\n';
          }
        }
        else
          os << "%global debug_package %{nil}"                          << '\n';

        // Common arguments for build2 commands.
        //
        // Let's use absolute path to the build system driver in case we are
        // invoked with altered environment or some such.
        //
        // Note: should be consistent with the invocation in installed_entries()
        //       above.
        //
        cstrings verb_args; string verb_arg;
        map_verb_b (*ops_, verb_b::normal, verb_args, verb_arg);

        os <<   '\n'
           <<   "%global build2 " << search_b (*ops_).effect_string ();
        for (const char* o: verb_args) os << ' ' << o;
        for (const string& o: ops_->build_option ()) os << ' ' << o;

        // Map the %{_smp_build_ncpus} macro value to the build2 --jobs or
        // --serial-stop options.
        //
        os <<   '\n'
           <<   '\n'
           <<   "%if %{defined _smp_build_ncpus}"                       << '\n'
           <<   "  %if %{_smp_build_ncpus} == 1"                        << '\n'
           <<   "    %global build2 %{build2} --serial-stop"            << '\n'
           <<   "  %else"                                               << '\n'
           <<   "    %global build2 %{build2} --jobs=%{_smp_build_ncpus}" << '\n'
           <<   "  %endif"                                              << '\n'
           <<   "%endif"                                                << '\n';

        // Configuration variables.
        //
        // Note: we need to quote values that contain `<>`, `[]`, since they
        // will be passed through shell. For simplicity, let's just quote
        // everything.
        //
        os <<   '\n'
           <<   "%global config_vars";

        auto add_macro_line = [&os] (const auto& v)
        {
          os << " \\\\\\\n  " << v;
        };

        add_macro_line ("config.install.chroot='%{buildroot}/'");
        add_macro_line ("config.install.sudo='[null]'");

        // If this is a C-based language, add rpath for private installation.
        //
        if (priv && (lang_c || lang_cxx || lang_cc))
          add_macro_line ("config.bin.rpath='%{_libdir}/" + pn.string () + "/'");

        // Add build flags.
        //
        if (ops_->fedora_buildflags () != "ignore")
        {
          const string& m (ops_->fedora_buildflags ());

          string o (m == "assign"  ? "="  :
                    m == "append"  ? "+=" :
                    m == "prepend" ? "=+" : "");

          if (o.empty ())
            fail << "unknown --fedora-buildflags option value '" << m << "'";

          // Note that config.cc.* doesn't play well with the append/prepend
          // modes because the orders are:
          //
          //  x.poptions cc.poptions
          // cc.coptions  x.coptions
          // cc.loptions  x.loptions
          //
          // Oh, well, hopefully it will be close enough for most cases.
          //
          // Note also that there are compiler mode options that are not
          // overridden. Also the preprocessor options are normally contained
          // in the %{build_cflags} and %{build_cxxflags} macro definitions
          // and have no separate macros associated at this level (see "Using
          // RPM build flags" documentation for details). For example:
          //
          // $ rpm --eval "%{build_cflags}"
          // -O2 -flto=auto -ffat-lto-objects -fexceptions -g
          // -grecord-gcc-switches -pipe -Wall -Werror=format-security
          // -Wp,-D_FORTIFY_SOURCE=2 -Wp,-D_GLIBCXX_ASSERTIONS
          // -specs=/usr/lib/rpm/redhat/redhat-hardened-cc1
          // -fstack-protector-strong -specs=/usr/lib/rpm/redhat/redhat-annobin-cc1
          // -m64  -mtune=generic -fasynchronous-unwind-tables
          // -fstack-clash-protection -fcf-protection
          //
          // Note the -Wp options above. Thus, we reset config.{c,cxx}.poptions
          // to [null] in the assign mode and, for simplicity, leave them as
          // configured otherwise. We could potentially fix that either by
          // extracting the -Wp,... options from %{build_cflags} and
          // %{build_cxxflags} macro values or using more lower level macros
          // instead (%{_preprocessor_defines}, %{_hardened_cflags}, etc),
          // which all feels quite hairy and brittle.
          //
          if (o == "=" && (lang_c || lang_cxx || lang_cc))
          {
            add_macro_line ("config.cc.poptions='[null]'");
            add_macro_line ("config.cc.coptions='[null]'");
            add_macro_line ("config.cc.loptions='[null]'");
          }

          if (lang_c || lang_cc)
          {
            if (o == "=")
              add_macro_line ("config.c.poptions='[null]'");

            add_macro_line ("config.c.coptions" + o + "'%{?build_cflags}'");
            add_macro_line ("config.c.loptions" + o + "'%{?build_ldflags}'");
          }

          if (lang_cxx || lang_cc)
          {
            if (o == "=")
              add_macro_line ("config.cxx.poptions='[null]'");

            add_macro_line ("config.cxx.coptions" + o + "'%{?build_cxxflags}'");
            add_macro_line ("config.cxx.loptions" + o + "'%{?build_ldflags}'");
          }
        }

        // Keep last to allow user-specified configuration variables to
        // override anything.
        //
        for (const string& c: config)
        {
          // Quote the value unless already quoted (see above). Presense of
          // potentially-quoted user variables complicates things a bit (can
          // be partially quoted, double-quoted, etc).
          //
          size_t p (c.find_first_of ("=+ \t")); // End of name.
          if (p != string::npos)
          {
            p = c.find_first_not_of ("=+ \t", p); // Beginning of value.
            if (p != string::npos)
            {
              if (c.find_first_of ("'\"", p) == string::npos) // Not quoted.
              {
                add_macro_line (string (c, 0, p) + '\'' + string (c, p) + '\'');
                continue;
              }
            }
          }

          add_macro_line (c);
        }

        os <<   '\n'; // Close the macro definition.

        // List of packages we need to install.
        //
        os <<   '\n'
           <<   "%global packages";

        for (const package& p: pkgs)
          add_macro_line (p.out_root.representation ());

        os <<   '\n'; // Close the macro definition.
      }

      // Build sections.
      //
      {
        os <<   '\n'
           <<   "# Build sections."                                     << '\n'
           <<   "#"                                                     << '\n'
           <<   "%prep"                                                 << '\n'
           <<   "%setup -T -c"                                          << '\n'
           <<   '\n'
           <<   "%build"                                                << '\n'
           <<   "%{build2} %{config_vars} update-for-install: %{packages}" << '\n'
           <<   '\n'
           <<   "%install"                                              << '\n'
           <<   "%{build2} %{config_vars} '!config.install.scope=" << scope
           <<   "' install: %{packages}"                                << '\n';
      }

      // Files sections.
      //
      // Generate the %files section for each sub-package in order to sort out
      // which files belong where.
      //
      // For the details on the %files section directives see "Directives For
      // the %files list" documentation. But the summary is:
      //
      //   - Supports only simple wildcards (?, *, [...]; no recursive/**).
      //   - Includes directories recursively, unless the path is prefixed
      //     with the %dir directive, in which case only includes this
      //     directory entry, which will be created on install and removed on
      //     uninstall, if empty.
      //   - An entry that doesn't match anything is an error (say,
      //     /usr/sbin/*).
      //
      // Keep in mind that wherever there is <project> in the config.install.*
      // variable, we can end up with multiple different directories (bundled
      // packages).
      //
      {
        string main;
        string devel;
        string static_;
        string doc;
        string common;

        // Note that declaring package ownership for standard directories is
        // considered in Fedora a bad idea and is reported as an error by some
        // RPM package checking tools (rpmlint, etc). Thus, we generate, for
        // example, libexecdir/* entry rather than libexecdir/. However, if
        // the private directory is specified we generate libexecdir/<private>/
        // to own the directory.
        //
        // NOTE: use consistently with the above install directory expressions
        //       (%{?_includedir}, etc) evaluation.
        //
        string pd (priv ? pn.string () + '/' : "");

        // The main package contains everything that doesn't go to another
        // packages.
        //
        if (gen_main)
        {
          if (ies.contains_sub (bindir))  main += "%{_bindir}/*\n";
          if (ies.contains_sub (sbindir)) main += "%{_sbindir}/*\n";

          if (ies.contains_sub (libexecdir))
            main += "%{_libexecdir}/" + (priv ? pd : "*") + '\n';

          // This could potentially go to -common but it could also be target-
          // specific, who knows. So let's keep it in main for now.
          //
          // Let's also specify that the confdir/ sub-entries are
          // non-replacable configuration files. This, in particular, means
          // that if edited they will not be replaced/removed on the package
          // upgrade or uninstallation (see RPM Packaging Guide for more
          // details on the %config(noreplace) directive). Also note that the
          // binary package configuration files can later be queried by the
          // user via the `rpm --query --configfiles` command.
          //
          if (ies.contains_sub (confdir))
            main += "%config(noreplace) %{_sysconfdir}/*\n";
        }

        if (ies.contains_sub (incdir))
          (!st.devel.empty () ? devel : main) +=
            "%{_includedir}/" + (priv ? pd : "*") + '\n';

        if (st.devel.empty () && st.static_.empty ())
        {
          assert (gen_main); // Shouldn't be here otherwise.

          if (ies.contains_sub (libdir))
            main += "%{_libdir}/" + (priv ? pd : "*") + '\n';
        }
        else
        {
          // Ok, time for things to get hairy: we need to split the contents
          // of lib/ into the main, -devel, and/or -static sub-packages. The
          // -devel sub-package, if present, should contain three things:
          //
          // 1. Static libraries (.a), if no -static sub-package is present.
          // 2. Non-versioned shared library symlinks (.so).
          // 3. Contents of the pkgconfig/ subdirectory, except for *.static.pc
          //    files if -static sub-package is present.
          //
          // The -static sub-package, if present, should contain two things:
          //
          // 1. Static libraries (.a).
          // 2. *.static.pc files in pkgconfig/ subdirectory.
          //
          // Everything else should go into the main package.
          //
          // The shared libraries are tricky. Here we can have three plausible
          // arrangements:
          //
          // A. Portably-versioned library:
          //
          //    libfoo-1.2.so
          //    libfoo.so     -> libfoo-1.2.so
          //
          // B. Natively-versioned library:
          //
          //    libfoo.so.1.2.3
          //    libfoo.so.1.2   -> libfoo.so.1.2.3
          //    libfoo.so.1     -> libfoo.so.1.2
          //    libfoo.so       -> libfoo.so.1
          //
          // C. Non-versioned library:
          //
          //    libfoo.so
          //
          // Note that in the (C) case the library should go into the main
          // package. Based on this, the criteria appears to be
          // straightforward: the extension is .so and it's a symlink. For
          // good measure we also check that there is the `lib` prefix
          // (plugins, etc).
          //
          // Also note that if <private>/ is specified, then to establish
          // ownership of the libdir/<private>/ directory we also need to add
          // it non-recursively to one of the potentially 3 sub-packages,
          // which all can contain some of its sub-entries. Not doing this
          // will result in an empty libdir/<private>/ subdirectory after the
          // binary package uninstallation. Naturally, the owner should be the
          // right-most sub-package on the following diagram which contains
          // any of the libdir/<private>/ sub-entries:
          //
          // -static -> -devel -> main
          //
          // The same reasoning applies to libdir/<private>/pkgconfig/.
          //
          string* owners[] = {&static_, &devel, &main};

          // Indexes (in owners) of sub-packages which should own
          // libdir/<private>/ and libdir/<private>/pkgconfig/. If nullopt,
          // then no additional directory ownership entry needs to be added
          // (installation is not private, recursive directory entry is
          // already added, etc).
          //
          optional<size_t> private_owner;
          optional<size_t> pkgconfig_owner;

          for (auto p (ies.find_sub (libdir)); p.first != p.second; )
          {
            const path& f (p.first->first);
            const installed_entry& ie ((p.first++)->second);

            path l (f.leaf (libdir));
            const string& n (l.string ());
            string* fs (&main); // Go to the main package as a last resort.

            auto update_ownership = [&owners, &fs] (optional<size_t>& pi)
            {
              size_t i (0);
              for (; owners[i] != fs; ++i) ;

              if (!pi || *pi < i)
                pi = i;
            };

            if (l.simple ())
            {
              if (n.size () > 3 && n.compare (0, 3, "lib") == 0)
              {
                string e (l.extension ());

                if (e == "a")
                {
                  fs = !st.static_.empty () ? &static_ : &devel;
                }
                else if (e == "so" && ie.target != nullptr &&
                         !st.devel.empty ())
                {
                  fs = &devel;
                }
              }

              *fs += "%{_libdir}/" + pd + n + '\n';
            }
            else
            {
              // Let's keep things tidy and, when possible, use a
              // sub-directory rather than listing all its sub-entries
              // verbatim.
              //
              dir_path sd (*l.begin ());
              dir_path d (libdir / sd);

              if (d == pkgdir)
              {
                // If the -static sub-package is not being generated, then the
                // whole directory goes into the -devel sub-package.
                // Otherwise, *.static.pc files go into the -static
                // sub-package and the rest into the -devel sub-package,
                // unless it is not being generated in which case it goes into
                // the main package.
                //
                if (!st.static_.empty ())
                {
                  if (n.size () > 10 &&
                      n.compare (n.size () - 10, 10, ".static.pc") == 0)
                    fs = &static_;
                  else if (!st.devel.empty ())
                    fs = &devel;

                  *fs += "%{_libdir}/" + pd + n;

                  // Update the index of a sub-package which should own
                  // libdir/<private>/pkgconfig/.
                  //
                  if (priv)
                    update_ownership (pkgconfig_owner);
                }
                else
                {
                  fs = &devel;
                  *fs += "%{_libdir}/" + pd + sd.string () + (priv ? "/" : "/*");
                }
              }
              else
                *fs += "%{_libdir}/" + pd + sd.string () + '/';

              // In the case of the directory (has the trailing slash) or
              // wildcard (has the trailing asterisk) skip all the other
              // entries in this subdirectory (in the prefix map they will all
              // be in a contiguous range).
              //
              char c (fs->back ());

              if (c == '/' || c == '*')
              {
                while (p.first != p.second && p.first->first.sub (d))
                  ++p.first;
              }

              *fs += '\n';
            }

            // We can only add files to the main package if we generate it.
            //
            assert (fs != &main || gen_main);

            // Update the index of a sub-package which should own
            // libdir/<private>/.
            //
            if (priv)
              update_ownership (private_owner);
          }

          // Add the directory ownership entries.
          //
          if (private_owner)
            *owners[*private_owner] += "%dir %{_libdir}/" + pd + '\n';

          if (pkgconfig_owner)
            *owners[*pkgconfig_owner] +=
              "%dir %{_libdir}/" + pd + "pkgconfig/" + '\n';
        }

        // We cannot just do usr/share/* since it will clash with doc/, man/,
        // and licenses/ below. So we have to list all the top-level entries
        // in usr/share/ that are not doc/, man/, licenses/, or build2/.
        //
        if (gen_main)
        {
          // Note that if <private>/ is specified, then we also need to
          // establish ownership of the sharedir/<private>/ directory (similar
          // to what we do for libdir/<private>/ above).
          //
          string* private_owner (nullptr);

          string& fs (!st.common.empty () ? common : main);

          for (auto p (ies.find_sub (sharedir)); p.first != p.second; )
          {
            const path& f ((p.first++)->first);

            if (f.sub (docdir)     ||
                f.sub (mandir)     ||
                f.sub (licensedir) ||
                f.sub (build2dir))
              continue;

            path l (f.leaf (sharedir));

            if (l.simple ())
            {
              fs += "%{_datadir}/" + pd + l.string () + '\n';
            }
            else
            {
              // Let's keep things tidy and use a sub-directory rather than
              // listing all its sub-entries verbatim.
              //
              dir_path sd (*l.begin ());

              fs += "%{_datadir}/" + pd + sd.string () + '/' + '\n';

              // Skip all the other entries in this subdirectory (in the prefix
              // map they will all be in a contiguous range).
              //
              dir_path d (sharedir / sd);
              while (p.first != p.second && p.first->first.sub (d))
                ++p.first;
            }

            // Indicate that we need to establish ownership of
            // sharedir/<private>/.
            //
            if (priv)
              private_owner = &fs;
          }

          // Add the directory ownership entry.
          //
          if (private_owner != nullptr)
            *private_owner += "%dir %{_datadir}/" + pd + '\n';
        }

        // Note that we only consider the bfdir/<project>/* sub-entries,
        // adding the bfdir/<project>/ subdirectories to the %files
        // section. This way no additional directory ownership entry needs to
        // be added. Any immediate sub-entries of bfdir/, if present, will be
        // ignored, which will end up with the 'unpackaged files' rpmbuild
        // error.
        //
        // Also note that the bfdir/ directory is not owned by any package.
        //
        if (gen_main)
        {
          for (auto p (ies.find_sub (bfdir)); p.first != p.second; )
          {
            const path& f ((p.first++)->first);

            path l (f.leaf (bfdir));

            if (!l.simple ())
            {
              // Let's keep things tidy and use a sub-directory rather than
              // listing all its sub-entries verbatim.
              //
              dir_path sd (*l.begin ());

              main += "%{_datadir}/build2/export/" + sd.string () + '/' + '\n';

              // Skip all the other entries in this subdirectory (in the
              // prefix map they will all be in a contiguous range).
              //
              dir_path d (bfdir / sd);
              while (p.first != p.second && p.first->first.sub (d))
                ++p.first;
            }
          }
        }

        // Should we put the documentation into -common if there is no -doc?
        // While there doesn't seem to be anything explicit in the policy,
        // there are packages that do it this way (e.g., mariadb-common). And
        // the same logic seems to apply to -devel (e.g., zlib-devel).
        //
        {
          string& fs (!st.doc.empty ()    ? doc    :
                      !st.common.empty () ? common :
                      !st.devel.empty ()  ? devel  :
                      main);

          // We can only add doc files to the main or -common packages if we
          // generate the main package.
          //
          assert ((&fs != &main && &fs != &common) || gen_main);

          // Let's specify that the docdir/ sub-entries are documentation
          // files. Note that the binary package documentation files can later
          // be queried by the user via the `rpm --query --docfiles` command.
          //
          if (ies.contains_sub (docdir))
            fs += "%doc %{_docdir}/" + (priv ? pd : "*") + '\n';

          // Since the man file may not appear directly in the man/
          // subdirectory we use the man/*/* wildcard rather than man/* not to
          // declare ownership for standard directories.
          //
          // As a side note, rpmbuild compresses the man files in the
          // installation directory, which needs to be taken into account if
          // writing more specific wildcards (e.g., %{_mandir}/man1/foo.1*).
          //
          if (ies.contains_sub (mandir))
            fs += "%{_mandir}/*/*\n";
        }

        // Let's specify that the licensedir/ sub-entries are license files.
        // Note that the binary package license files can later be queried by
        // the user via the `rpm --query --licensefiles` command.
        //
        if (ies.contains_sub (licensedir))
          (gen_main ? main : devel) +=
            "%license %{_licensedir}/" + (priv ? pd : "*") + '\n';

        // Finally, write the %files sections.
        //
        if (!main.empty ())
        {
          assert (gen_main); // Shouldn't be here otherwise.

          os << '\n'
             << "# " << st.main << " files."                            << '\n'
             << "#"                                                     << '\n'
             << "%files"                                                << '\n'
             << main;
        }

        if (!devel.empty ())
        {
          os << '\n'
             << "# " << st.devel << " files."                           << '\n'
             << "#"                                                     << '\n'
             << "%files -n " << st.devel                                << '\n'
             << devel;
        }

        if (!static_.empty ())
        {
          os << '\n'
             << "# " << st.static_ << " files."                         << '\n'
             << "#"                                                     << '\n'
             << "%files -n " << st.static_                              << '\n'
             << static_;
        }

        if (!doc.empty ())
        {
          os << '\n'
             << "# " << st.doc << " files."                             << '\n'
             << "#"                                                     << '\n'
             << "%files -n " << st.doc                                  << '\n'
             << doc;
        }

        if (!common.empty ())
        {
          os << '\n'
             << "# " << st.common << " files."                          << '\n'
             << "#"                                                     << '\n'
             << "%files -n " << st.common                               << '\n'
             << common;
        }
      }

      // Changelog section.
      //
      // The section entry has the following format:
      //
      // * <day-of-week> <month> <day> <year> <name> <surname> <email> - <version>-<release>
      // - <change1-description>
      // - <change2-description>
      // ...
      //
      // For example:
      //
      // * Wed Feb 22 2023 John Doe <john@example.com> - 2.3.4-1
      // - New bpkg package release 2.3.4.
      //
      // We will use the Packager value for the `<name> <surname> <email>`
      // fields. Strictly speaking it may not exactly match the fields set but
      // it doesn't seem to break anything if that's the case. For good
      // measure, me will also use the English locale for the date.
      //
      // Note that the <release> field doesn't contain the distribution tag.
      //
      {
        os <<   '\n'
           <<   "%changelog"                                            << '\n'
           <<   "* ";

        // Given that we don't include the timezone there is no much sense to
        // print the current time as local.
        //
        std::locale l (os.imbue (std::locale ("C")));
        to_stream (os,
                   system_clock::now (),
                   "%a %b %d %Y",
                   false /* special */,
                   false /* local */);
        os.imbue (l);

        os << ' ' << packager << " - ";

        if (!sys_version.epoch.empty ())
          os << sys_version.epoch << ':';

        os << sys_version.version << '-' << sys_version.release         << '\n'
           <<   "- New bpkg package release " << pvs << '.'             << '\n';
      }

      os.close ();
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << spec << ": " << e;
    }

    // Run rpmbuild.
    //
    // Note that rpmbuild causes recompilation periodically by setting the
    // SOURCE_DATE_EPOCH environment variable (which we track for changes
    // since it affects GCC). Its value depends on the timestamp of the latest
    // change log entry and thus has a day resolution. Note that since we
    // don't have this SOURCE_DATE_EPOCH during dry-run caused by
    // installed_entries(), there would be a recompilation even if the value
    // weren't changing.
    //
    cstrings args {"rpmbuild", "-bb"}; // Only build binary packages.

    // Map our verbosity to rpmbuild --quiet and -vv options (-v is the
    // default). Note that there doesn't seem to be any way to control its
    // progress.
    //
    // Also note that even in the quiet mode rpmbuild may still print some
    // progress lines.
    //
    if (verb == 0)
      args.push_back ("--quiet");
    else if (verb >= 4) // Note that -vv feels too verbose for level 3.
      args.push_back ("-vv");

    // If requested, keep the installation directory, etc.
    //
    if (ops_->keep_output ())
      args.push_back ("--noclean");

    // Pass our --jobs value, if any.
    //
    string jobs_arg;
    if (size_t n = ops_->jobs_specified () ? ops_->jobs () : 0)
    {
      jobs_arg = "--define=_smp_build_ncpus " + to_string (n);
      args.push_back (jobs_arg.c_str ());
    }

    // Pass the rpmbuild/rpm common options.
    //
    for (const string& o: common_opts)
      args.push_back (o.c_str ());

    // Pass any additional options specified by the user.
    //
    for (const string& o: ops_->fedora_build_option ())
      args.push_back (o.c_str ());

    args.push_back (spec.string ().c_str ());
    args.push_back (nullptr);

    if (ops_->fedora_prepare_only ())
    {
      if (verb >= 1)
      {
        diag_record dr (text);

        dr << "prepared " << spec <<
          text << "command line: ";

        print_process (dr, args);
      }

      return binary_files {};
    }

    try
    {
      process_path pp (process::path_search (args[0]));
      process_env pe (pp);

      // There is going to be quite a bit of diagnostics so print the command
      // line unless quiet.
      //
      if (verb >= 1)
        print_process (pe, args);

      // Redirect stdout to stderr since some of rpmbuild diagnostics goes
      // there. For good measure also redirect stdin to /dev/null to make sure
      // there are no prompts of any kind.
      //
      process pr (pp, args, -2 /* stdin */, 2 /* stdout */, 2 /* stderr */);

      if (!pr.wait ())
      {
        // Let's repeat the command line even if it was printed at the
        // beginning to save the user a rummage through the logs.
        //
        diag_record dr (fail);
        dr << args[0] << " exited with non-zero code" <<
          info << "command line: "; print_process (dr, pe, args);
      }
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }

    // While it's tempting to always keep the spec file let's remove it,
    // unless requested not to, since it contains absolute paths to
    // configuration.
    //
    if (!ops_->keep_output ())
      rm (spec);

    // Collect and return the binary sub-package paths.
    //
    // Here we will use `rpm --eval` to resolve the RPM sub-package paths.
    //
    binary_files r;
    r.system_version = st.system_version;
    {
      string expressions;

      auto add_macro = [&expressions] (const string& name, const string& value)
      {
        expressions += "%global " + name + ' ' + value + '\n';
      };

      add_macro ("VERSION", sys_version.version);
      add_macro ("RELEASE", sys_version.release + "%{?dist}");

      const string& package_arch (!build_arch.empty () ? build_arch : arch);

      vector<binary_file> files;

      auto add_package = [&files, &expressions, &rpmfile, &add_macro]
                         (const string& name,
                          const string& arch,
                          const char* type) -> size_t
      {
        add_macro ("NAME", name);
        add_macro ("ARCH", arch);
        expressions += rpmfile + '\n';

        // Note: path is unknown yet.
        //
        files.push_back (binary_file {type, path (), name});
        return files.size () - 1;
      };

      if (gen_main)
        add_package (st.main, package_arch, "main.rpm");

      if (!st.devel.empty ())
        add_package (st.devel, package_arch, "devel.rpm");

      if (!st.static_.empty ())
        add_package (st.static_, package_arch, "static.rpm");

      if (!st.doc.empty ())
        add_package (st.doc, "noarch", "doc.rpm");

      if (!st.common.empty ())
        add_package (st.common, "noarch", "common.rpm");

      optional<size_t> di (
        !binless
        ? add_package (st.main + "-debuginfo", arch, "debuginfo.rpm")
        : optional<size_t> ());

      // Strip the trailing newline since rpm adds one.
      //
      expressions.pop_back ();

      strings expansions (eval (cstrings ({expressions.c_str ()})));

      if (expansions.size () != files.size ())
        fail << "number of RPM file path expansions differs from number "
             << "of path expressions";

      for (size_t i (0); i != files.size(); ++i)
      {
        try
        {
          path p (move (expansions[i]));

          if (p.empty ())
            throw invalid_path ("");

          // Note that the -debuginfo sub-package may potentially not be
          // generated (no installed binaries to extract the debug info from,
          // etc).
          //
          if (exists (p))
          {
            binary_file& f (files[i]);

            r.push_back (
              binary_file {move (f.type), move (p), move (f.system_name)});
          }
          else if (!di || i != *di) // Not a -debuginfo sub-package?
            fail << "expected output file " << p << " does not exist";
        }
        catch (const invalid_path& e)
        {
          fail << "invalid path '" << e.path << "' in RPM file path expansions";
        }
      }
    }

    return r;
  }
}
