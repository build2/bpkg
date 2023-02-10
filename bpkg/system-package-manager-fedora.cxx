// file      : bpkg/system-package-manager-fedora.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager-fedora.hxx>

#include <bpkg/diagnostics.hxx>

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

  // Parse the fedora-name (or alike) value.
  //
  // Note that for now we treat all the packages from the non-main groups as
  // extras omitting the -common package (assuming it's pulled by the main
  // package) as well as -doc and -debug* unless requested with the
  // extra_{doc,debug*} arguments. Note that we treat -static as -devel (since
  // we can't know whether the static library is needed or not).
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
      // to end with -devel (e.g., libfoo-devel libfoo-devel-devel).
      //
      {
        string& m (ns[0]);

        if (pn != nullptr                            &&
            pn->string ().compare (0, 3, "lib") == 0 &&
            pn->string ().size () > 3                &&
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
        r = parse_group (gs[i], &pn);
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
    // end up with the -libs subpackage rather than with the base package as,
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
                          const string& qarch)
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
    const char* args[] = {
      "dnf", "repoquery", "--requires",
      "--quiet",
      "--cacheonly", // Don't automatically update the metadata.
      "--resolve",   // Resolve requirements to packages/versions.
      "--qf", "%{name} %{arch} %{epoch}:%{version}-%{release}",
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
        simulation::package k {name, ver, qarch};

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

  // Execute `dnf install` to install the specified packages (e.g., libfoo or
  // libfoo-1.2.3-1.fc35.x86_64).
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
  // <name> form until any matched packages are found (see SPECIFYING PACKAGES
  // section of dnf(8) for more details on the spec matching rules). We could
  // potentially use `dnf install-nevra` command for the package version specs
  // and `dnf install-n` for the package name specs. Let's, however, keep it
  // simple for now given that clashes for our use-case are presumably not
  // very likely.
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
          dr << info << "while mapping " << pn << " to " << os_release.name_id
             << " package name";
        });

      strings ns;
      if (!aps->empty ())
        ns = system_package_names (*aps,
                                   os_release.name_id,
                                   os_release.version_id,
                                   os_release.like_ids);
      if (ns.empty ())
      {
        // Attempt to automatically translate our package name. Failed that we
        // should try to use the project name, if present, as a fallback.
        //
        const string& n (pn.string ());

        // Note that theoretically different available packages can have
        // different project names. But taking it form the latest version
        // feels good enough.
        //
        const shared_ptr<available_package>& ap (!aps->empty ()
                                                 ? aps->front ().first
                                                 : nullptr);

        string f (ap != nullptr && ap->project && *ap->project != pn
                  ? ap->project->string ()
                  : empty_string);

        // The best we can do in trying to detect whether this is a library is
        // to check for the lib prefix. Libraries without the lib prefix and
        // non-libraries with the lib prefix (both of which we do not
        // recomment) will have to provide a manual mapping.
        //
        if (n.compare (0, 3, "lib") == 0 && n.size () > 3)
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
    // architecture.
    //
    auto guess_main = [this, &pn] (package_status& s,
                                   const string& ver,
                                   const string& qarch)
    {
      vector<pair<string, string>> depends (
        dnf_repoquery_requires (s.devel, ver, qarch));

      s.main = main_from_devel (s.devel, ver, depends);

      if (s.main.empty ())
      {
        diag_record dr (fail);
        dr << "unable to guess main " << os_release.name_id
           << " package for " << s.devel << ' ' << ver <<
          info << "depends on";


        for (auto b (depends.begin ()), i (b); i != depends.end (); ++i)
        {
          dr << (i == b ? " " : ", ") << i->first << ' ' << i->second;
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
          dr << fail << "multiple installed " << os_release.name_id
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
              if (!fp.candidate_version.empty ())
              {
                // Use the fallback.
                //
                (ps.main.empty () ? ps.devel : ps.main) = move (ps.fallback);
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
                ps.main.clear ();
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
            dr << fail << "multiple available " << os_release.name_id
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
      // release from Fedora version ([<epoch>:]<version>-<release>).
      //
      string sv (r->system_version, 0, r->system_version.rfind ('-'));

      optional<version> v;
      if (!aps->empty ())
        v = downstream_package_version (sv,
                                        *aps,
                                        os_release.name_id,
                                        os_release.version_id,
                                        os_release.like_ids);

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

    // Collect and merge all the Fedora packages/versions for the specified
    // bpkg packages.
    //
    struct package
    {
      string name;
      string version; // Empty if unspecified.
      string arch;    // Empty if version is empty.
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
    // version, expecting the candidate version to be installed.
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
        string a (fi ? pi.installed_arch    : string ());

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
        s += '.';
        s += p.arch;
      }
      specs.push_back (move (s));
    }

    // Install.
    //
    if (install)
      dnf_install (specs);

    // Mark as installed by the user.
    //
    dnf_mark_install (specs);

    // Verify that versions we have promised in pkg_status() match what
    // actually got installed.
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

  void system_package_manager_fedora::
  generate (packages&&,
            packages&&,
            strings&&,
            const dir_path&,
            optional<recursive_mode>)
  {
  }
}
