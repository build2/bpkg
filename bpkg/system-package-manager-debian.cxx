// file      : bpkg/system-package-manager-debian.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager-debian.hxx>

#include <locale>

#include <libbutl/timestamp.hxx>
#include <libbutl/filesystem.hxx> // permissions

#include <bpkg/diagnostics.hxx>

#include <bpkg/pkg-bindist-options.hxx>

using namespace butl;

namespace bpkg
{
  using package_status = system_package_status_debian;

  // Translate host CPU to Debian package architecture.
  //
  string system_package_manager_debian::
  arch_from_target (const target_triplet& h)
  {
    const string& c (h.cpu);
    return
      c == "x86_64"                                            ? "amd64" :
      c == "aarch64"                                           ? "arm64" :
      c == "i386" || c == "i486" || c == "i586" || c == "i686" ? "i386"  :
      c;
  }

  // Parse the debian-name (or alike) value. The first argument is the package
  // type.
  //
  // Note that for now we treat all the packages from the non-main groups as
  // extras omitting the -common package (assuming it's pulled by the main
  // package) as well as -doc and -dbg unless requested with the
  // extra_{doc,dbg} arguments.
  //
  package_status system_package_manager_debian::
  parse_name_value (const string& pt,
                    const string& nv,
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

    auto parse_group = [&split, &suffix] (const string& g, const string* pt)
    {
      strings ns (split (g, ' '));

      if (ns.empty ())
        fail << "empty package group";

      package_status r;

      // Handle the "dev instead of main" special case for libraries.
      //
      // Check that the following name does not end with -dev. This will be
      // the only way to disambiguate the case where the library name happens
      // to end with -dev (e.g., libfoo-dev libfoo-dev-dev).
      //
      {
        string& m (ns[0]);

        if (pt != nullptr      &&
            *pt == "lib"       &&
            suffix (m, "-dev") &&
            !(ns.size () > 1 && suffix (ns[1], "-dev")))
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

    package_status r;
    for (size_t i (0); i != gs.size (); ++i)
    {
      if (i == 0) // Main group.
        r = parse_group (gs[i], &pt);
      else
      {
        package_status g (parse_group (gs[i], nullptr));

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

  // Attempt to determine the main package name from its -dev package based on
  // the extracted Depends value. Return empty string if unable to.
  //
  string system_package_manager_debian::
  main_from_dev (const string& dev_name,
                 const string& dev_ver,
                 const string& depends)
  {
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
  // Note also that for some reason both apt-cache and apt-get exit with 100
  // code on error.
  //
  static process_path apt_cache_path;
  static process_path apt_get_path;
  static process_path sudo_path;

  // Obtain the installed and candidate versions for the specified list of
  // Debian packages by executing `apt-cache policy`.
  //
  // If the n argument is not 0, then only query the first n packages.
  //
  void system_package_manager_debian::
  apt_cache_policy (vector<package_policy>& pps, size_t n)
  {
    if (n == 0)
      n = pps.size ();

    assert (n != 0 && n <= pps.size ());

    // The --quiet option makes sure we don't get a noice (N) printed to
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
      if (apt_cache_path.empty () && !simulate_)
        apt_cache_path = process::path_search (args[0], false /* init */);

      process_env pe (apt_cache_path, evars);

      if (verb >= 3)
        print_process (pe, args);

      // Redirect stdout to a pipe. For good measure also redirect stdin to
      // /dev/null to make sure there are no prompts of any kind.
      //
      process pr;
      if (!simulate_)
        pr = process (apt_cache_path,
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
      }

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
            for (; i != n && pps[i].name != l; ++i) ;

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

            // Candidate should fallback to Installed.
            //
            assert (pp.installed_version.empty () ||
                    !pp.candidate_version.empty ());

            // Skip the rest of the indented lines (or blanks, just in case).
            //
            while (!eof (getline (is, l)) && (l.empty () || l.front () == ' ')) ;
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

  // Execute `apt-cache show` and return the Depends value, if any, for the
  // specified package and version. Fail if either package or version is
  // unknown.
  //
  string system_package_manager_debian::
  apt_cache_show (const string& name, const string& ver)
  {
    assert (!name.empty () && !ver.empty ());

    string spec (name + '=' + ver);

    // In particular, --quiet makes sure we don't get noices (N) printed to
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
      if (apt_cache_path.empty () && !simulate_)
        apt_cache_path = process::path_search (args[0], false /* init */);

      process_env pe (apt_cache_path, evars);

      if (verb >= 3)
        print_process (pe, args);

      // Redirect stdout to a pipe. For good measure also redirect stdin to
      // /dev/null to make sure there are no prompts of any kind.
      //
      process pr;
      if (!simulate_)
        pr = process (apt_cache_path,
                      args,
                      -2      /* stdin */,
                      -1      /* stdout */,
                      2       /* stderr */,
                      nullptr /* cwd */,
                      evars);
      else
      {
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
      }

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
            // This line should be the start of a field unless it's a comment
            // or the terminating blank line. According to deb822(5), there
            // can be no leading whitespaces before `#`.
            //
            if (l.empty ())
              break;

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
          fail << "unable to read " << args[0] << " show output: " << e;

        // Fall through.
      }

      if (!pr.wait () || no_version)
      {
        diag_record dr (fail);
        dr << args[0] << " show exited with non-zero code";

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

  // Prepare the common `apt-get <command>` options.
  //
  pair<cstrings, const process_path&> system_package_manager_debian::
  apt_get_common (const char* command, strings& args_storage)
  {
    // Pre-allocate the required number of entries in the arguments storage.
    //
    if (fetch_timeout_)
      args_storage.reserve (1);

    cstrings args;

    if (!sudo_.empty ())
      args.push_back (sudo_.c_str ());

    args.push_back ("apt-get");
    args.push_back (command);

    // Map our verbosity/progress to apt-get --quiet[=<level>]. The levels
    // appear to have the following behavior:
    //
    // 1 -- shows URL being downloaded but no percentage progress is shown.
    //
    // 2 -- only shows diagnostics (implies --assume-yes which cannot be
    //      overriden with --assume-no).
    //
    // It also appears to automatically use level 1 if stderr is not a
    // terminal. This can be overrident with --quiet=0.
    //
    // Note also that --show-progress does not apply to apt-get update. For
    // apt-get install it shows additionally progress during unpacking which
    // looks quite odd.
    //
    if (progress_ && *progress_)
    {
      args.push_back ("--quiet=0");
    }
    else if (verb == 0)
    {
      // Only use level 2 if assuming yes.
      //
      args.push_back (yes_ ? "--quiet=2" : "--quiet");
    }
    else if (progress_ && !*progress_)
    {
      args.push_back ("--quiet");
    }

    if (yes_)
    {
      args.push_back ("--assume-yes");
    }
    else if (!stderr_term)
    {
      // Suppress any prompts if stderr is not a terminal for good measure.
      //
      args.push_back ("--assume-no");
    }

    // Add the network operations timeout options, if requested.
    //
    if (fetch_timeout_)
    {
      args.push_back ("-o");

      args_storage.push_back (
        "Acquire::http::Timeout=" + to_string (*fetch_timeout_));

      args.push_back (args_storage.back ().c_str ());
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
        if (apt_get_path.empty () && !simulate_)
          apt_get_path = process::path_search (args[0], false /* init */);

        pp = &apt_get_path;
      }

      return pair<cstrings, const process_path&> (move (args), *pp);
    }
    catch (const process_error& e)
    {
      fail << "unable to execute " << args[0] << ": " << e << endf;
    }
  }

  // Execute `apt-get update` to update the package index.
  //
  void system_package_manager_debian::
  apt_get_update ()
  {
    strings args_storage;
    pair<cstrings, const process_path&> args_pp (
      apt_get_common ("update", args_storage));

    cstrings& args (args_pp.first);
    const process_path& pp (args_pp.second);

    args.push_back (nullptr);

    try
    {
      if (verb >= 2)
        print_process (args);
      else if (verb == 1)
        text << "updating " << os_release.name_id << " package index...";

      process pr;
      if (!simulate_)
      {
        // Redirect stdout to stderr since apt-get prints some of its
        // diagnostics to stdout.
        //
        pr = process (pp, args, 0 /* stdin */, 2 /* stdout */);
      }
      else
      {
        print_process (args);
        pr = process (process_exit (simulate_->apt_get_update_fail_ ? 100 : 0));
      }

      if (!pr.wait ())
      {
        diag_record dr (fail);
        dr << "apt-get update exited with non-zero code";

        if (verb < 2)
        {
          dr << info << "command line: ";
          print_process (dr, args);
        }
      }

      if (verb == 1)
        text << "updated " << os_release.name_id << " package index";
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  // Execute `apt-get install` to install the specified packages/versions
  // (e.g., libfoo or libfoo=1.2.3).
  //
  void system_package_manager_debian::
  apt_get_install (const strings& pkgs)
  {
    assert (!pkgs.empty ());

    strings args_storage;
    pair<cstrings, const process_path&> args_pp (
      apt_get_common ("install", args_storage));

    cstrings& args (args_pp.first);
    const process_path& pp (args_pp.second);

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
        // Redirect stdout to stderr since apt-get prints some of its
        // diagnostics to stdout.
        //
        pr = process (pp, args, 0 /* stdin */, 2 /* stdout */);
      }
      else
      {
        print_process (args);
        pr = process (process_exit (simulate_->apt_get_install_fail_ ? 100 : 0));
      }

      if (!pr.wait ())
      {
        diag_record dr (fail);
        dr << "apt-get install exited with non-zero code";

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

  optional<const system_package_status*> system_package_manager_debian::
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

  optional<package_status> system_package_manager_debian::
  status (const package_name& pn, const available_packages& aps)
  {
    tracer trace ("system_package_manager_debian::status");

    // For now we ignore -doc and -dbg package components (but we may want to
    // have options controlling this later). Note also that we assume -common
    // is pulled automatically by the main package so we ignore it as well
    // (see equivalent logic in parse_name_value()).
    //
    bool need_doc (false);
    bool need_dbg (false);

    vector<package_status> candidates;

    // Translate our package name to the Debian package names.
    //
    {
      auto df = make_diag_frame (
        [this, &pn] (diag_record& dr)
        {
          dr << info << "while mapping " << pn << " to "
             << os_release.name_id << " package name";
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
        // Attempt to automatically translate our package name (see above for
        // details).
        //
        const string& n (pn.string ());

        if (pt == "lib")
        {
          // Keep the main package name empty as an indication that it is to
          // be discovered.
          //
          // @@ It seems that quite often the header-only library -dev package
          //    name doesn't start with 'lib'. Here are some randomly chosen
          //    packages: libeigen3-dev, libmdds-dev, rapidjson-dev, etl-dev,
          //    seqan-dev, catch2. Should we implement the fallback similar to
          //    the Fedora implementation? Maybe one day.
          //
          candidates.push_back (package_status ("", n + "-dev"));
        }
        else
          candidates.push_back (package_status (n));
      }
      else
      {
        // Parse each manual mapping.
        //
        for (const string& n: ns)
        {
          package_status s (parse_name_value (pt, n, need_doc, need_dbg));

          // Suppress duplicates for good measure based on the main package
          // name (and falling back to -dev if empty).
          //
          auto i (find_if (candidates.begin (), candidates.end (),
                           [&s] (const package_status& x)
                           {
                             // Note that it's possible for one mapping to be
                             // specified as -dev only while the other as main
                             // and -dev.
                             //
                             return s.main.empty () || x.main.empty ()
                               ? s.dev == x.dev
                               : s.main == x.main;
                           }));
          if (i == candidates.end ())
            candidates.push_back (move (s));
          else
          {
            // Should we verify the rest matches for good measure? But what if
            // we need to override, as in:
            //
            // debian_10-name: libcurl4 libcurl4-openssl-dev
            // debian_9-name: libcurl4 libcurl4-dev
            //
            // Note that for this to work we must get debian_10 values before
            // debian_9, which is the semantics guaranteed by
            // system_package_names().
          }
        }
      }
    }

    // Guess unknown main package given the -dev package and its version.
    // Failed that, assume the package to be a binless library and leave the
    // main member of the package_status object empty.
    //
    auto guess_main = [this, &trace] (package_status& s, const string& ver)
    {
      string depends (apt_cache_show (s.dev, ver));

      s.main = main_from_dev (s.dev, ver, depends);

      if (s.main.empty ())
      {
        l4 ([&]{trace << "unable to guess main package for " << s.dev << ' '
                      << ver << ", Depends value: " << depends;});
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

          // Note that at this stage we can only use the installed -dev
          // package (since the candidate version may change after fetch).
          //
          if (dev.installed_version.empty ())
            continue;

          guess_main (ps, dev.installed_version);

          if (!ps.main.empty ()) // Not a binless library?
          {
            pps.emplace (pps.begin (), ps.main);
            ps.package_policies_main++;
            apt_cache_policy (pps, 1);
          }
        }

        optional<status_type> s (status (pps, ps.package_policies_main));

        if (!s || *s != package_status::installed)
          continue;

        const package_policy& main (pps.front ()); // Main/dev.

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
    // the non-installable candidates by setting both their main and -dev
    // package names to empty strings.
    //
    if (!r && install_)
    {
      // If we weren't instructed to fetch or we already fetched, then we
      // don't need to re-run apt_cache_policy().
      //
      bool requery;
      if ((requery = fetch_ && !fetched_))
      {
        apt_get_update ();
        fetched_ = true;
      }

      {
        diag_record dr; // Ambiguity diagnostics.

        for (package_status& ps: candidates)
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
            {
              // Not installable.
              //
              ps.dev.clear ();
              continue;
            }

            guess_main (ps, dev.candidate_version);

            if (!ps.main.empty ()) // Not a binless library?
            {
              pps.emplace (pps.begin (), ps.main);
              ps.package_policies_main++;
              apt_cache_policy (pps, 1);
            }
          }

          optional<status_type> s (status (pps, ps.package_policies_main));

          if (!s)
          {
            // Not installable.
            //
            ps.main.clear ();
            ps.dev.clear ();
            continue;
          }

          assert (*s != package_status::installed); // Sanity check.

          const package_policy& main (pps.front ()); // Main/dev.

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
            for (const package_policy& pp: s.package_policies)
              if (pp.installed_version.empty ())
                dr << ' ' << pp.name;
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
          if (ps.main.empty () && ps.dev.empty ()) // Not installable?
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
      // Map the Debian version to the bpkg version. But first strip the
      // revision from Debian version ([<epoch>:]<upstream>[-<revision>]), if
      // any.
      //
      // Note that according to deb-version(5), <upstream> may contain `:`/`-`
      // but in these cases <epoch>/<revision> must be specified explicitly,
      // respectively.
      //
      string sv (r->system_version, 0, r->system_version.rfind ('-'));

      // Debian package versions sometimes include metadata introduced with
      // the `+` character that established relationships between Debian and
      // upstream packages, backports, and some other murky stuff (see Debian
      // Policy with a strong cup of coffee for details). Normally such
      // metadata is included in the revision, as in, for example,
      // 1.4.0-1+deb10u1. However, sometimes you see it included in the
      // version directly, as in, for example: 3.2.4+debian-1 (see Debian bug
      // 542288 for a potential explanation; might need to refill you cup).
      //
      // Since our upstream version may not contain `+`, it feels reasonable
      // to remove such metadata. What's less clear is whether we should do
      // the same before trying the to-downstream mapping. In fact, it's
      // possible `+` belongs to the upstream version rather than Debian
      // metadata or that there are both (there is also no guarantee that
      // Debian metadata may not contain multiple `+`). So what we are going
      // to do is try the mapping with more and more `+` components stripped
      // (naturally ending up with all of them stripped for the fallback
      // below).
      //
      optional<version> v;
      for (size_t p (sv.size ()); p != string::npos; p = sv.rfind ('+'))
      {
        sv.resize (p);

        if (!aps.empty ())
        {
          v = downstream_package_version (sv,
                                          aps,
                                          os_release.name_id,
                                          os_release.version_id,
                                          os_release.like_ids);
          if (v)
            break;
        }
      }

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

  void system_package_manager_debian::
  install (const vector<package_name>& pns)
  {
    assert (!pns.empty ());

    assert (install_ && !installed_);
    installed_ = true;

    // Collect and merge all the Debian packages/versions for the specified
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

      apt_get_install (specs);
    }

    // Verify that versions we have promised in status() match what actually
    // got installed.
    //
    {
      vector<package_policy> pps;

      // Here we just check the main package component of each package.
      //
      for (const package_name& pn: pns)
      {
        const package_status& ps (*status_cache_.find (pn)->second);

        if (find_if (pps.begin (), pps.end (),
                     [&ps] (const package_policy& pp)
                     {
                       return pp.name == ps.system_name;
                     }) == pps.end ())
        {
          pps.push_back (package_policy (ps.system_name));
        }
      }

      apt_cache_policy (pps);

      for (const package_name& pn: pns)
      {
        const package_status& ps (*status_cache_.find (pn)->second);

        auto i (find_if (pps.begin (), pps.end (),
                         [&ps] (const package_policy& pp)
                         {
                           return pp.name == ps.system_name;
                         }));
        assert (i != pps.end ());

        const package_policy& pp (*i);

        if (pp.installed_version != ps.system_version)
        {
          fail << "unexpected " << os_release.name_id << " package version "
               << "for " << ps.system_name <<
            info << "expected: " << ps.system_version <<
            info << "installed: " << pp.installed_version <<
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
  // mapping is specified, we will generate libsqlite3 and libsqlite3-dev
  // while native names are libsqlite3-0 and libsqlite3-dev. While this
  // duality is not ideal, presumably we will normally only be producing our
  // binary packages if there are no suitable native packages. And for a few
  // exceptions (e.g., our package is "better" in some way, such as configured
  // differently or fixes a critical bug), we will just have to provide
  // appropriate manual mapping that makes sure the names match (the extras is
  // still a potential problem though -- we will only have them as
  // dependencies if we build against a native system package; maybe we can
  // add them manually with an option).
  //
  package_status system_package_manager_debian::
  map_package (const package_name& pn,
               const version& pv,
               const available_packages& aps,
               const optional<string>& build_metadata) const
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
      // case above. Except here we don't attempt to deduce main from -dev,
      // naturally.
      //
      const string& n (pn.string ());

      if (pt == "lib")
        r = package_status (n, n + "-dev");
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
                            false /* need_dbg */);

      // If this is -dev without main, then derive main by stripping the -dev
      // suffix. This feels tighter than just using the bpkg package name.
      //
      if (r.main.empty ())
      {
        assert (!r.dev.empty ());
        r.main.assign (r.dev, 0, r.dev.size () - 4);
      }
    }

    // Map the version.
    //
    // NOTE: THE BELOW DESCRIPTION IS ALSO REPRODUCED IN THE BPKG MANUAL.
    //
    // To recap, a Debian package version has the following form:
    //
    // [<epoch>:]<upstream>[-<revision>]
    //
    // For details on the ordering semantics, see the Version control file
    // field documentation in the Debian Policy Manual. While overall
    // unsurprising, one notable exception is `~`, which sorts before anything
    // else and is commonly used for upstream pre-releases. For example,
    // 1.0~beta1~svn1245 sorts earlier than 1.0~beta1, which sorts earlier
    // than 1.0.
    //
    // There are also various special version conventions (such as all the
    // revision components in 1.4-5+deb10u1~bpo9u1) but they all appear to
    // express relationships between native packages and/or their upstream and
    // thus do not apply to our case.
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
    //   Debian's native package epoch. But on the other it will allow our
    //   binary packages from different epochs to co-exist. Seeing that this
    //   can be easily overridden with a custom distribution version, let's
    //   keep it.
    //
    //   Note that while the Debian start/default epoch is 0, ours is 1 (we
    //   use the 0 epoch for stub packages). So we will need to shift this
    //   value range.
    //
    //
    // <upstream>[-<prerel>]
    //
    //   Our upstream version maps naturally to Debian's. That is, our
    //   upstream version format/semantics is a subset of Debian's.
    //
    //   If this is a pre-release, then we could fail (that is, don't allow
    //   pre-releases) but then we won't be able to test on pre-release
    //   packages, for example, to make sure the name mapping is correct.
    //   Plus sometimes it's useful to publish pre-releases. We could ignore
    //   it, but then such packages will be indistinguishable from each other
    //   and the final release, which is not ideal. On the other hand, Debian
    //   has the mechanism (`~`) which is essentially meant for this, so let's
    //   use it. We will use <prerel> as is since its format is the same as
    //   upstream and thus should map naturally.
    //
    //
    // <revision>
    //
    //   Similar to epoch, our revision won't necessarily match Debian's
    //   native package revision. But on the other hand it will allow us to
    //   establish a correspondence between source and binary packages. Plus,
    //   upgrades between binary package revisions will be handled naturally.
    //   Seeing that we allow overriding the revision with a custom
    //   distribution version (see below), let's keep it.
    //
    //   Note also that both Debian and our revision start/default is 0.
    //   However, it is Debian's convention to start revision from 1. But it
    //   doesn't seem worth it for us to do any shifting here and so we will
    //   use our revision as is.
    //
    //   Another related question is whether we should also include some
    //   metadata that identifies the distribution and its version that this
    //   package is for. The strongest precedent here is probably Ubuntu's
    //   PPA. While there doesn't appear to be a consistent approach, one can
    //   often see versions like these:
    //
    //   2.1.0-1~ppa0~ubuntu14.04.1,
    //   1.4-5-1.2.1~ubuntu20.04.1~ppa1
    //   22.12.2-0ubuntu1~ubuntu23.04~ppa1
    //
    //   Seeing that this is a non-sortable component (what in semver would be
    //   called "build metadata"), using `~` is probably not the worst choice.
    //
    //   So we follow this lead and add the ~<name_id><version_id> component
    //   to revision. Note that this also means we will have to make the 0
    //   revision explicit. For example:
    //
    //   1.2.3-1~debian10
    //   1.2.3-0~ubuntu20.04
    //
    // The next case to consider is when we have the upstream version
    // (upstream-version manifest value). After some rumination it feels
    // correct to use it in place of the <epoch>-<upstream> components in the
    // above mapping (upstream version itself cannot have epoch). In other
    // words, we will add the pre-release and revision components from the
    // bpkg version. If this is not the desired semantics, then it can always
    // be overrided with the distribution version.
    //
    // Finally, we have the distribution version. The Debian <epoch> and
    // <upstream> components are straightforward: they should be specified by
    // the distribution version as required. This leaves pre-release and
    // revision. It feels like in most cases we would want these copied over
    // from the bpkg version automatically -- it's too tedious and error-
    // prone to maintain them manually. However, we want the user to have the
    // full override ability. So instead, if empty revision is specified, as
    // in 1.2.3-, then we automatically add the bpkg revision. Similarly, if
    // empty pre-release is specified, as in 1.2.3~, then we add the bpkg
    // pre-release. To add both automatically, we would specify 1.2.3~- (other
    // combinations are 1.2.3~b.1- and 1.2.3~-1).
    //
    // Note also that per the Debian version specification, if upstream
    // contains `:` and/or `-`, then epoch and/or revision must be specified
    // explicitly, respectively. Note that the bpkg upstream version may not
    // contain either.
    //
    string& sv (r.system_version);

    bool no_build_metadata (build_metadata && build_metadata->empty ());

    if (optional<string> ov = system_package_version (ap,
                                                      rf,
                                                      os_release.name_id,
                                                      os_release.version_id,
                                                      os_release.like_ids))
    {
      string& dv (*ov);
      size_t n (dv.size ());

      // Find the revision and pre-release positions, if any.
      //
      size_t rp (dv.rfind ('-'));
      size_t pp (dv.rfind ('~', rp));

      // Copy over the [<epoch>:]<upstream> part.
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

      // Add revision copying over the bpkg version value if empty.
      //
      // Omit the default -0 revision if we have no build metadata.
      //
      if (rp != string::npos)
      {
        if (size_t rn = n - (rp + 1))
        {
          sv.append (dv, rp, rn + 1);
        }
        else if (pv.revision || !no_build_metadata)
        {
          sv += '-';
          sv += to_string (pv.revision ? *pv.revision : 0);
        }
      }
      else if (!no_build_metadata)
        sv += "-0"; // Default revision (for build metadata; see below).
    }
    else
    {
      if (ap->upstream_version)
      {
        const string& uv (*ap->upstream_version);

        // Add explicit epoch if upstream contains `:`.
        //
        // Note that we don't need to worry about `-` since we always add
        // revision (see below).
        //
        if (uv.find (':') != string::npos)
          sv = "0:";

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
      if (pv.revision || !no_build_metadata)
      {
        sv += '-';
        sv += to_string (pv.revision ? *pv.revision : 0);
      }
    }

    // Add build matadata.
    //
    if (!no_build_metadata)
    {
      sv += '~';

      if (!build_metadata)
      {
        sv += os_release.name_id;
        sv += os_release.version_id; // Could be empty.
      }
      else
      {
        const string& md (*build_metadata);

        bool f (md.front () == '+');
        bool b (md.back () == '+');

        if (f && b) // Note: covers just `+`.
          fail << "invalid build metadata '" << md << "'";

        if (f || b)
        {
          if (b)
            sv.append (md, 0, md.size () - 1);

          sv += os_release.name_id;
          sv += os_release.version_id;

          if (f)
            sv.append (md, 1, md.size () - 1);
        }
        else
          sv += md;
      }
    }

    return r;
  }

  // Some background on creating Debian packages (for a bit more detailed
  // overview see the Debian Packaging Tutorial).
  //
  // A binary Debian package (.deb) is an ar archive which itself contains a
  // few tar archives normally compressed with gz or xz. So it's possible to
  // create the package completely manually without using any of the Debian
  // tools and while some implementations (for example, cargo-deb) do it this
  // way, we are not going to go this route because it does not scale well to
  // more complex packages which may require additional functionality (such as
  // managing systemd files) and which is covered by the Debian tools (for an
  // example of where this leads, see the partial debhelper re-implementation
  // in cargo-deb). Another issues with this approach is that it's not
  // amenable to customizations, at least not in a way familiar to Debian
  // users.
  //
  // At the lowest level of the Debian tools for creating packages sits the
  // dpkg-deb --build|-b command (also accessible as dpkg --build|-b). Given a
  // directory with all the binary package contents (including the package
  // metadata, such as the control file, in the debian/ subdirectory) this
  // command will pack everything up into a .deb file. While an improvement
  // over the fully manual packaging, this approach has essentially the same
  // drawbacks. In particular, this command generates a single package which
  // means we will have to manually sort out things into -dev, -doc, etc.
  //
  // Next up the stack is dpkg-buildpackage. This tool expects the package to
  // follow the Debian way of packaging, that is, to provide the debian/rules
  // makefile with a number of required targets which it then invokes to
  // build, install, and pack a package from source (and sometime during this
  // process it calls dpkg-deb --build). The dpkg-buildpackage(1) man page has
  // an overview of all the steps that this command performs and it is the
  // recommended, lower-level, way to build packages on Debian.
  //
  // At the top of the stack sits debuild which calls dpkg-buildpackage, then
  // lintian, and finally design (though signing can also be performed by
  // dpkg-buildpackage itself).
  //
  // Based on this our plan is to use dpkg-buildpackage which brings us to the
  // Debian way of packaging with debian/rules at its core. As it turns out,
  // it can also be implemented in a number of alternative ways. So let's
  // discuss those.
  //
  // As mentioned earlier, debian/rules is a makefile that is expected to
  // provide a number of targets, such as build, install, etc. And
  // theoretically these targets can be implemented completely manually. In
  // practice, however, the Debian way is to use the debhelper(1) packaging
  // helper tools. For example, there are helpers for stripping binaries,
  // compressing man pages, fixing permissions, and managing systemd files.
  //
  // While debhelper tools definitely simplify debian/rules, there is often
  // still a lot of boilerplate code. So second-level helpers are often used,
  // with the dominant option being the dh(1) command sequencer (there is also
  // CDBS but it appears to be fading into obsolescence).
  //
  // Based on that our options appear to be classic debhelper and dh. Looking
  // at the statistics, it's clear that the majority of packages (including
  // fairly complex ones) tend to prefer dh and there is no reason for us to
  // try to buck this trend.
  //
  // NOTE: THE BELOW DESCRIPTION IS ALSO REWORDED IN BPKG-PKG-BINDIST(1).
  //
  // So, to sum up, the plan is to produce debian/rules that uses the dh
  // command sequencer and then invoke dpkg-buildpackage to produce the binary
  // package from that. While this approach is normally used to build things
  // from source, it feels like we should be able to pretend that we are.
  // Specifially, we can override the install target to invoke the build
  // system and install all the packages directly from their bpkg locations.
  //
  // Note that the -dbgsym packages are generated by default and all we need
  // to do from our side is to compile with debug information (-g), failed
  // which we get a warning from debhelper.
  //
  // Note: this setup requires dpkg-dev (or build-essential) and debhelper
  // packages.
  //
  auto system_package_manager_debian::
  generate (const packages& pkgs,
            const packages& deps,
            const strings& vars,
            const dir_path& cfg_dir,
            const package_manifest& pm,
            const string& pt,
            const small_vector<language, 1>& langs,
            optional<bool> recursive_full,
            bool first) -> binary_files
  {
    tracer trace ("system_package_manager_debian::generate");

    assert (!langs.empty ()); // Should be effective.

    // We require explicit output root.
    //
    if (!ops_->output_root_specified ())
      fail << "output root directory must be specified explicitly with "
           << "--output-root|-o";

    const dir_path& out (ops_->output_root ()); // Cannot be empty.

    optional<string> build_metadata;
    if (ops_->debian_build_meta_specified ())
      build_metadata = ops_->debian_build_meta ();

    const shared_ptr<selected_package>& sp (pkgs.front ().selected);
    const package_name& pn (sp->name);
    const version& pv (sp->version);

    // Use version without iteration in paths, etc (`#` breaks dpkg
    // machinery).
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
    package_status st (map_package (pn, pv, aps, build_metadata));

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
      }
      else
      {
        s = map_package (sp->name, sp->version, aps, build_metadata);

        // Set the main package name to an empty string if we wouldn't be
        // generating the main package for this dependency (binless library
        // without the -common package).
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

    if (!st.dbg.empty ())
      fail << "generation of obsolete manual -dbg packages not supported" <<
        info << "use automatic -dbgsym packages instead";

    // We override every config.install.* variable in order not to pick
    // anything configured. Note that we add some more in the rules file
    // below.
    //
    // We make use of the <project> substitution since in the recursive mode
    // we may be installing multiple projects. Note that the <private>
    // directory component is automatically removed if this functionality is
    // not enabled. One side-effect of using <project> is that we will be
    // using the bpkg package name instead of the main Debian package name.
    // But perhaps that's correct: on Debian it's usually the source package
    // name, which is the same. To keep things consistent we use the bpkg
    // package name for <private> as well.
    //
    // Note that some libraries have what looks like architecture-specific
    // configuration files in /usr/include/$(DEB_HOST_MULTIARCH)/ which is
    // what we use for our config.install.include_arch location.
    //
    // Note: we need to quote values that contain `$` so that they don't get
    // expanded as build2 variables in the installed_entries() call.
    //
    // NOTE: make sure to update .install files below if changing anything
    //       here.
    //
    strings config {
      "config.install.root=/usr/",
      "config.install.data_root=root/",
      "config.install.exec_root=root/",

      "config.install.bin=exec_root/bin/",
      "config.install.sbin=exec_root/sbin/",

      // On Debian shared libraries should not be executable. Also,
      // libexec/ is the same as lib/ (note that executables that get
      // installed there will still have the executable bit set).
      //
      "config.install.lib='exec_root/lib/$(DEB_HOST_MULTIARCH)/<private>/'",
      "config.install.lib.mode=644",
      "config.install.libexec=lib/<project>/",
      "config.install.pkgconfig=lib/pkgconfig/",

      "config.install.etc=/etc/",
      "config.install.include=data_root/include/<private>/",
      "config.install.include_arch='data_root/include/$(DEB_HOST_MULTIARCH)/<private>/'",
      "config.install.share=data_root/share/",
      "config.install.data=share/<private>/<project>/",
      "config.install.buildfile=share/build2/export/<project>/",

      "config.install.doc=share/doc/<private>/<project>/",
      "config.install.legal=doc/",
      "config.install.man=share/man/",
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

    // Note that we can use weak install scope for the auto recursive mode
    // since we know dependencies cannot be spread over multiple linked
    // configurations.
    //
    string scope (!recursive_full || *recursive_full ? "project" : "weak");

    // Get the map of files that will end up in the binary packages.
    //
    // Note that we are passing quoted values with $(DEB_HOST_MULTIARCH) which
    // will be treated literally.
    //
    installed_entry_map ies (installed_entries (*ops_, pkgs, config, scope));

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

    // Installed entry directories for sorting out which files belong where.
    //
    // Let's tighten things up and only look in <private>/ (if specified) to
    // make sure there is nothing stray.
    //
    string pd (priv ? pn.string () + '/' : "");

    // NOTE: keep consistent with the config.install.* values above.
    //
    // We put exported buildfiles into the main package, which makes sense
    // after some meditation: they normally contain rules and are bundled
    // either with a tool (say, thrift), a module (say, libbuild2-thrift), or
    // an add-on package (say, thrift-build2).
    //
    dir_path bindir     ("/usr/bin/");
    dir_path sbindir    ("/usr/sbin/");
    dir_path etcdir     ("/etc/");
    dir_path incdir     ("/usr/include/" + pd);
    dir_path incarchdir ("/usr/include/$(DEB_HOST_MULTIARCH)/" + pd);
    //dir_path bfdir      ("/usr/share/build2/export/");
    dir_path libdir     ("/usr/lib/$(DEB_HOST_MULTIARCH)/" + pd);
    dir_path pkgdir     (libdir / dir_path ("pkgconfig"));
    dir_path sharedir   ("/usr/share/" + pd);
    dir_path docdir     ("/usr/share/doc/" + pd);
    dir_path mandir     ("/usr/share/man/");

    // As an optimization, don't generate the main and -dbgsym packages for a
    // binless library unless it also specifies the -common package.
    //
    // If this is a binless library, then verify that it doesn't install any
    // executable, library, or configuration files. Also verify that it has
    // the -dev package.
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

        // It would probably be better not to fail here but generate the main
        // package instead (as we do if the -common package is also being
        // generated). Then, however, it would not be easy to detect if a
        // dependency has the main package or not (see sdeps initialization
        // for details).
        //
        verify_not_installed (etcdir);

        for (auto p (ies.find_sub (libdir)); p.first != p.second; ++p.first)
        {
          const path& f (p.first->first);

          if (!f.sub (pkgdir))
            bad_install (f.string ());
        }

        // Verify packages.
        //
        if (st.dev.empty ())
          fail << "binless library " << pn << ' ' << pv << " doesn't have "
               << os_release.name_id << " -dev package";

        binless = true;
      }
    }

    bool gen_main (!binless || !st.common.empty ());

    // If we don't generate the main package (and thus the -common package),
    // then fail if there are any data files installed. It would probably be
    // better not to fail but generate the main package instead in this
    // case. Then, however, it would not be easy to detect if a dependency has
    // the main package or not.
    //
    if (!gen_main)
    {
      // Note: covers bfdir.
      //
      for (auto p (ies.find_sub (sharedir)); p.first != p.second; ++p.first)
      {
        const path& f (p.first->first);

        if (!f.sub (docdir) && !f.sub (mandir))
        {
          fail << "binless library " << pn << ' ' << pv << " installs " << f <<
            info << "consider specifying -common package in explicit "
                 << os_release.name_id << " name mapping in package manifest";
        }
      }
    }

    if (verb >= 3)
    {
      auto print_status = [] (diag_record& dr,
                              const package_status& s,
                              const string& main)
      {
        dr << (main.empty () ? "" : " ") << main
           << (s.dev.empty () ? "" : " ") << s.dev
           << (s.doc.empty () ? "" : " ") << s.doc
           << (s.dbg.empty () ? "" : " ") << s.dbg
           << (s.common.empty () ? "" : " ") << s.common
           << ' ' << s.system_version;
      };

      {
        diag_record dr (trace);
        dr << "package:";
        print_status (dr, st, gen_main ? st.main : empty_string);
      }

      for (const package_status& st: sdeps)
      {
        diag_record dr (trace);
        dr << "dependency:";
        print_status (dr, st, st.main);
      }
    }

    // Start assembling the package "source" directory.
    //
    // It's hard to predict all the files that will be generated (and
    // potentially read), so we will just require a clean output directory.
    //
    // Also, by default, we are going to keep all the intermediate files on
    // failure for troubleshooting.
    //
    if (first && exists (out) && !empty (out))
    {
      if (!ops_->wipe_output ())
        fail << "output root directory " << out << " is not empty" <<
          info << "use --wipe-output to clean it up but be careful";

      rm_r (out, false);
    }

    // Normally the source directory is called <name>-<upstream-version>
    // (e.g., as unpacked from the source archive).
    //
    dir_path src (out / dir_path (pn.string () + '-' + pvs));
    dir_path deb (src / dir_path ("debian"));
    mk_p (deb);

    // The control file.
    //
    // See the "Control files and their fields" chapter in the Debian Policy
    // Manual for details (for example, which fields are mandatory).
    //
    // Note that we try to do a reasonably thorough job (e.g., filling in
    // sections, etc) with the view that this can be used as a starting point
    // for manual packaging (and perhaps we could add a mode for this in the
    // future, call it "starting point" mode).
    //
    // Also note that this file supports variable substitutions (for example,
    // ${binary:Version}) as described in deb-substvars(5). While we could do
    // without, it is widely used in manual packages so we do the same. Note,
    // however, that we don't use the shlibs:Depends/misc:Depends mechanism
    // (which automatically detects dependencies) since we have an accurate
    // set and some of them may not be system packages.
    //
    string homepage (pm.package_url ? pm.package_url->string () :
                     pm.url         ? pm.url->string ()         :
                     string ());

    string maintainer;
    if (ops_->debian_maintainer_specified ())
      maintainer = ops_->debian_maintainer ();
    else
    {
      const email* e (pm.package_email ? &*pm.package_email :
                      pm.email         ? &*pm.email         :
                      nullptr);

      if (e == nullptr)
        fail << "unable to determine package maintainer from manifest" <<
          info << "specify explicitly with --debian-maintainer";

      // In certain places (e.g., changelog), Debian expect this to be in the
      // `John Doe <john@example.org>` form while we often specify just the
      // email address (e.g., to the mailing list). Try to detect such a case
      // and complete it to the desired format.
      //
      if (e->find (' ') == string::npos && e->find ('@') != string::npos)
      {
        // Try to use comment as name, if any.
        //
        if (!e->comment.empty ())
          maintainer = e->comment;
        else
          maintainer = pn.string () + " package maintainer";

        maintainer += " <" + *e + '>';
      }
      else
        maintainer = *e;
    }

    path ctrl (deb / "control");
    try
    {
      ofdstream os (ctrl);

      // First comes the general (source package) stanza.
      //
      // Note that the Priority semantics is not the same as our priority.
      // Rather it should reflect the overall importance of the package. Our
      // priority is more appropriately mapped to urgency in the changelog.
      //
      // If this is not a library, then by default we assume its some kind of
      // a development tool and use the devel section.
      //
      // Note also that we require the debhelper compatibility level 13 which
      // has more advanced features that we rely on. Such as:
      //
      //   - Variable substitutions in the debhelper config files.
      //
      string section (
        ops_->debian_section_specified () ? ops_->debian_section () :
        lib                               ? "libs"                  :
        "devel");

      string priority (
        ops_->debian_priority_specified () ? ops_->debian_priority () :
        "optional");

      os <<   "Source: "              << pn                         << '\n'
         <<   "Section: "             << section                    << '\n'
         <<   "Priority: "            << priority                   << '\n'
         <<   "Maintainer: "          << maintainer                 << '\n'
         <<   "Standards-Version: "   << "4.6.2"                    << '\n'
         <<   "Build-Depends: "       << "debhelper-compat (= 13)"  << '\n'
         <<   "Rules-Requires-Root: " << "no"                       << '\n';
      if (!homepage.empty ())
        os << "Homepage: "            << homepage                   << '\n';
      if (pm.src_url)
        os << "Vcs-Browser: "         << pm.src_url->string ()      << '\n';

      // Then we have one or more binary package stanzas.
      //
      // Note that values from the source package stanza (such as Section,
      // Priority) are used as defaults for the binary packages.
      //
      // We cannot easily detect architecture-independent packages (think
      // libbutl.bash) and providing an option feels like the best we can do.
      // Note that the value `any` means architecture-dependent while `all`
      // means architecture-independent.
      //
      // The Multi-Arch hint can be `same` or `foreign`. The former means that
      // a separate copy of the package may be installed for each architecture
      // (e.g., library) while the latter -- that a single copy may be used by
      // all architectures (e.g., executable, -doc, -common). Note that for
      // some murky reasons Multi-Arch:foreign needs to be explicitly
      // specified for Architecture:all.
      //
      // The Description field is quite messy: it requires both the short
      // description (our summary) as a first line and a long description (our
      // description) as the following lines in the multiline format.
      // Converting our description to the Debian format is not going to be
      // easy: it can be arbitrarily long and may not even be plain text (it's
      // commonly the contents of the README.md file). So for now we fake it
      // with a description of the package component. Note also that
      // traditionally the Description field comes last.
      //
      string arch (ops_->debian_architecture_specified ()
                   ? ops_->debian_architecture ()
                   : "any");

      string march (arch == "all" || !lib ? "foreign" : "same");

      if (gen_main)
      {
        string depends;

        auto add_depends = [&depends] (const string& v)
        {
          if (!depends.empty ())
            depends += ", ";

          depends += v;
        };

        if (!st.common.empty ())
          add_depends (st.common + " (= ${binary:Version})");

        for (const package_status& st: sdeps)
        {
          // Note that the constraints will include build metadata (e.g.,
          // ~debian10). While it may be tempting to strip it, we cannot since
          // the order is inverse. We could just make it empty `~`, though
          // that will look a bit strange. But keeping it shouldn't cause any
          // issues. Also note that the build metadata is part of the revision
          // so we could strip the whole thing.
          //
          if (!st.main.empty ())
            add_depends (st.main + " (>= " + st.system_version + ')');
        }

        if (ops_->debian_main_langdep_specified ())
        {
          if (!ops_->debian_main_langdep ().empty ())
          {
            if (!depends.empty ())
              depends += ", ";

            depends += ops_->debian_main_langdep ();
          }
        }
        else
        {
          // Note that we are not going to add dependencies on libcN
          // (currently libc6) or libstdc++N (currently libstdc++6) because
          // it's not easy to determine N and they both are normally part of
          // the base system.
          //
          // What about other language runtimes? Well, it doesn't seem like we
          // can deduce those automatically so we will either have to add ad
          // hoc support or the user will have to provide them manually with
          // --debian-main-depends.
        }

        if (!ops_->debian_main_extradep ().empty ())
        {
          if (!depends.empty ())
            depends += ", ";

          depends += ops_->debian_main_extradep ();
        }

        os <<   '\n'
           <<   "Package: "      << st.main                  << '\n'
           <<   "Architecture: " << arch                     << '\n'
           <<   "Multi-Arch: "   << march                    << '\n';
        if (!depends.empty ())
          os << "Depends: "      << depends                  << '\n';
        os <<   "Description: "  << pm.summary               << '\n'
           <<   " This package contains the runtime files."  << '\n';
      }

      if (!st.dev.empty ())
      {
        string depends (gen_main ? st.main + " (= ${binary:Version})" : "");

        auto add_depends = [&depends] (const string& v)
        {
          if (!depends.empty ())
            depends += ", ";

          depends += v;
        };

        for (const package_status& st: sdeps)
        {
          // Doesn't look like we can distinguish between interface and
          // implementation dependencies here. So better to over- than
          // under-specify.
          //
          if (!st.dev.empty ())
            add_depends (st.dev + " (>= " + st.system_version + ')');
        }

        if (ops_->debian_dev_langdep_specified ())
        {
          if (!ops_->debian_dev_langdep ().empty ())
          {
            add_depends (ops_->debian_dev_langdep ());
          }
        }
        else
        {
          // Add dependency on libcN-dev and libstdc++-N-dev.
          //
          // Note: libcN-dev provides libc-dev and libstdc++N-dev provides
          // libstdc++-dev. While it would be better to depend on the exact
          // versions, determining N is not easy (and in case of listdc++
          // there could be multiple installed at the same time).
          //
          // Note that we haven't seen just libc-dev in any native packages,
          // it's always either libc6-dev or libc6-dev|libc-dev. So we will
          // see how it goes.
          //
          // If this is an undetermined C-common library, we assume it may be
          // C++ (better to over- than under-specify).
          //
          bool cc (lang ("cc", true));
          if (cc || (cc = lang ("c++", true))) add_depends ("libstdc++-dev");
          if (cc || (cc = lang ("c",   true))) add_depends ("libc-dev");
        }

        if (!ops_->debian_dev_extradep ().empty ())
        {
          add_depends (ops_->debian_dev_extradep ());
        }

        // Feels like the architecture should be the same as for the main
        // package.
        //
        os <<   '\n'
           <<   "Package: "      << st.dev                        << '\n'
           <<   "Section: "      << (lib ? "libdevel" : "devel")  << '\n'
           <<   "Architecture: " << arch                          << '\n'
           <<   "Multi-Arch: "   << march                         << '\n';
        if (!st.doc.empty ())
          os << "Suggests: "     << st.doc                        << '\n';
        if (!depends.empty ())
          os << "Depends: "      << depends                       << '\n';
        os <<   "Description: "  << pm.summary                    << '\n'
           <<   " This package contains the development files."   << '\n';
      }

      if (!st.doc.empty ())
      {
        os << '\n'
           << "Package: "      << st.doc                   << '\n'
           << "Section: "      << "doc"                    << '\n'
           << "Architecture: " << "all"                    << '\n'
           << "Multi-Arch: "   << "foreign"                << '\n'
           << "Description: "  << pm.summary               << '\n'
           << " This package contains the documentation."  << '\n';
      }

      // Keep this in case we want to support it in the "starting point" mode.
      //
      if (!st.dbg.empty () && !binless)
      {
        string depends (st.main + " (= ${binary:Version})");

        os <<   '\n'
           <<   "Package: "      << st.dbg                           << '\n'
           <<   "Section: "      << "debug"                          << '\n'
           <<   "Priority: "     << "extra"                          << '\n'
           <<   "Architecture: " << arch                             << '\n'
           <<   "Multi-Arch: "   << march                            << '\n';
        if (!depends.empty ())
          os << "Depends: "      << depends                          << '\n';
        os <<   "Description: "  << pm.summary                       << '\n'
           <<   " This package contains the debugging information."  << '\n';
      }

      if (!st.common.empty ())
      {
        // Generally, this package is not necessarily architecture-independent
        // (for example, it could contain something shared between multiple
        // binary packages produced from the same source package rather than
        // something shared between all the architectures of a binary
        // package). But seeing that we always generate one binary package,
        // for us it only makes sense as architecture-independent.
        //
        // It's also not clear what dependencies we can deduce for this
        // package. Assuming that it depends on all the dependency -common
        // packages is probably unreasonable.
        //
        os << '\n'
           << "Package: "      << st.common                                << '\n'
           << "Architecture: " << "all"                                    << '\n'
           << "Multi-Arch: "   << "foreign"                                << '\n'
           << "Description: "  << pm.summary                               << '\n'
           << " This package contains the architecture-independent files." << '\n';
      }

      os.close ();
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << ctrl << ": " << e;
    }

    // The changelog file.
    //
    // See the "Debian changelog" section in the Debian Policy Manual for
    // details.
    //
    // In particular, this is the sole source of the package version.
    //
    timestamp now (system_clock::now ());

    path chlog (deb / "changelog");
    try
    {
      ofdstream os (chlog);

      // The first line has the following format:
      //
      // <src-package> (<version>) <distribution>; urgency=<urgency>
      //
      // Note that <distribution> doesn't end up in the binary package.
      // Normally all Debian packages start in unstable or experimental.
      //
      string urgency;
      switch (pm.priority ? pm.priority->value : priority::low)
      {
      case priority::low:      urgency = "low";      break;
      case priority::medium:   urgency = "medium";   break;
      case priority::high:     urgency = "high";     break;
      case priority::security: urgency = "critical"; break;
      }

      os << pn << " (" << st.system_version << ") "
         << (pv.release ? "experimental" : "unstable") << "; "
         << "urgency=" << urgency << '\n';

      // Next we have a bunch of "change details" lines that start with `*`
      // indented with two spaces. They are traditionally seperated from the
      // first and last lines with blank lines.
      //
      os << '\n'
         << "  * New bpkg package release " << pvs << '.' << '\n'
         << '\n';

      // The last line is the "maintainer signoff" and has the following
      // form:
      //
      //  -- <name> <email>  <date>
      //
      // The <date> component shall have the following form in the English
      // locale (Mon, Jan, etc):
      //
      // <day-of-week>, <dd> <month> <yyyy> <hh>:<mm>:<ss> +<zzzz>
      //
      os << " -- " << maintainer << "  ";
      std::locale l (os.imbue (std::locale ("C")));
      to_stream (os,
                 now,
                 "%a, %d %b %Y %T %z",
                 false /* special */,
                 true  /* local */);
      os.imbue (l);
      os << '\n';

      os.close ();
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << chlog << ": " << e;
    }

    // The copyright file.
    //
    // See the "Machine-readable debian/copyright file" document for
    // details.
    //
    // Note that while not entirely clear, it looks like there should be at
    // least one Files stanza.
    //
    // Note also that there is currently no way for us to get accurate
    // copyright information.
    //
    // @@ TODO: Strictly speaking, in the recursive mode, we should collect
    //    licenses of all the dependencies we are bundling.
    //
    path copyr (deb / "copyright");
    try
    {
      ofdstream os (copyr);

      string license;
      for (const licenses& ls: pm.license_alternatives)
      {
        if (!license.empty ())
          license += " or ";

        for (auto b (ls.begin ()), i (b); i != ls.end (); ++i)
        {
          if (i != b)
            license += " and ";

          license += *i;
        }
      }

      os <<   "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/" << '\n'
         <<   "Upstream-Name: "    << pn          << '\n'
         <<   "Upstream-Contact: " << maintainer  << '\n';
      if (!homepage.empty ())
        os << "Source: "           << homepage    << '\n';
      os <<   "License: "          << license     << '\n'
         <<   "Comment: See accompanying files for exact copyright information" << '\n'
         <<   " and full license text(s)." << '\n';

      // Note that for licenses mentioned in the Files stanza we either have
      // to provide the license text(s) inline or as separate License stanzas.
      //
      os << '\n'
         << "Files: *" << '\n'
         << "Copyright: ";
      to_stream (os, now, "%Y", false /* special */, true  /* local */);
      os << " the " << pn << " authors (see accompanying files for details)" << '\n'
         << "License: " << license << '\n'
         << " See accompanying files for full license text(s)." << '\n';

      os.close ();
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << copyr << ": " << e;
    }

    // The source/format file.
    //
    dir_path deb_src (deb / dir_path ("source"));
    mk (deb_src);

    path format (deb_src / "format");
    try
    {
      ofdstream os (format);
      os << "3.0 (quilt)\n";
      os.close ();
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << format << ": " << e;
    }

    // The rules makefile. Note that it must be executable.
    //
    // This file is executed by dpkg-buildpackage(1) which expects it to
    // provide the following "API" make targets:
    //
    // clean
    //
    // build        -- configure and build for all package
    // build-arch   -- configure and build for Architecture:any packages
    // build-indep  -- configure and build for Architecture:all packages
    //
    // binary       -- make all binary packages
    // binary-arch  -- make Architecture:any binary packages
    // binary-indep -- make Architecture:all binary packages
    //
    // The dh command sequencer provides the standard implementation of these
    // API targets with the following customization point targets (for an
    // overview of dh, start with the slides from the "Not Your Grandpa's
    // Debhelper" presentation at DebConf 9 followed by the dh(1) man page):
    //
    // override_dh_auto_configure   # ./configure --prefix=/usr
    // override_dh_auto_build       # make
    // override_dh_auto_test        # make test
    // override_dh_auto_install     # make install
    // override_dh_auto_clean       # make distclean
    //
    // Note that pretty much any dh_xxx command invoked by dh in order to
    // implement the API targets can be customized with the corresponding
    // override_dh_xxx target. To see what commands are executed for an API
    // target, run `dh <target> --no-act`.
    //
    path rules (deb / "rules");
    try
    {
      bool lang_c   (lang ("c"));
      bool lang_cxx (lang ("c++"));
      bool lang_cc  (lang ("cc"));

      // See fdopen() for details (umask, etc).
      //
      permissions ps (permissions::ru | permissions::wu | permissions::xu |
                      permissions::rg | permissions::wg | permissions::xg |
                      permissions::ro | permissions::wo | permissions::xo);
      ofdstream os (fdopen (rules,
                            fdopen_mode::out | fdopen_mode::create,
                            ps));

      os << "#!/usr/bin/make -f\n"
         << "# -*- makefile -*-\n"
         << '\n';

      // See debhelper(7) for details on these.
      //
      // Note that there is also the DEB_BUILD_OPTIONS=terse option. Perhaps
      // for the "starting point" mode we should base DH_* values as well as
      // the build system verbosity below on that value. See debian/rules in
      // upstream mariadb for what looks like a sensible setup.
      //
      if (verb == 0)
        os << "export DH_QUIET := 1\n"
           << '\n';
      else if (verb == 1)
        os << "# Uncomment this to turn on verbose mode.\n"
           << "#export DH_VERBOSE := 1\n"
           << '\n';
      else
        os << "export DH_VERBOSE := 1\n"
           << '\n';

      // We could have instead called dpkg-architecture directly but seeing
      // that we are also include buildflags.mk below, might as well use
      // architecture.mk (in the packages that we sampled you see both
      // approaches). Note that these come in the dpkg-dev package, the same
      // as dpkg-buildpackage.
      //
      os << "# DEB_HOST_* (DEB_HOST_MULTIARCH, etc)"   << '\n'
         << "#"                                        << '\n'
         << "include /usr/share/dpkg/architecture.mk"  << '\n'
         << '\n';

      if (ops_->debian_buildflags () != "ignore")
      {
        // While we could have called dpkg-buildflags directly, including
        // buildflags.mk instead appears to be the standard practice.
        //
        // Note that theses flags are not limited to C-based languages (for
        // example, they also cover Assembler, Fortran, and potentially others
        // in the future).
        //
        string mo; // Include leading space if not empty.
        if (ops_->debian_maint_option_specified ())
        {
          for (const string& o: ops_->debian_maint_option ())
          {
            if (!o.empty ())
            {
              mo += ' ';
              mo += o;
            }
          }
        }
        else
          mo = " hardening=+all";

        os << "# *FLAGS (CFLAGS, CXXFLAGS, etc)"                  << '\n'
           << "#"                                                 << '\n'
           << "export DEB_BUILD_MAINT_OPTIONS :=" << mo           << '\n'
           << "include /usr/share/dpkg/buildflags.mk"             << '\n'
           << '\n';

        if (!binless)
        {
          // Fixup -ffile-prefix-map option (if specified) which is used to
          // strip source file path prefix in debug information (besides other
          // places). By default it points to the source directory. We change
          // it to point to the bpkg configuration directory. Note that this
          // won't work for external packages with source out of configuration
          // (e.g., managed by bdep).
          //
          if (lang_c || lang_cc)
          {
            // @@ TODO: OBJCFLAGS.

            os << "CFLAGS := $(patsubst -ffile-prefix-map=%,-ffile-prefix-map="
               << cfg_dir.string () << "=.,$(CFLAGS))" << '\n'
               << '\n';
          }

          if (lang_cxx || lang_cc)
          {
            // @@ TODO: OBJCXXFLAGS.

            os << "CXXFLAGS := $(patsubst -ffile-prefix-map=%,-ffile-prefix-map="
               << cfg_dir.string () << "=.,$(CXXFLAGS))" << '\n'
               << '\n';
          }
        }
      }

      // For a binless library the -dbgsym package is not supposed to be
      // generated. Thus, we disable its automatic generation by adding the
      // noautodbgsym flag to the DEB_BUILD_OPTIONS variable.
      //
      // This doesn't seem to be necessary (probably because there is no
      // .so/.a).
      //
#if 0
      if (binless)
        os << "export DEB_BUILD_OPTIONS += noautodbgsym" << '\n'
           << '\n';
#endif

      // The debian/tmp/ subdirectory appears to be the canonical destination
      // directory (see dh_auto_install(1) for details).
      //
      os << "DESTDIR := $(CURDIR)/debian/tmp" << '\n'
         << '\n';

      // Let's use absolute path to the build system driver in case we are
      // invoked with altered environment or some such.
      //
      // See --jobs documentation in dpkg-buildpackage(1) for details on
      // parallel=N.
      //
      // Note: should be consistent with the invocation in installed_entries()
      //       above.
      //
      cstrings verb_args; string verb_arg;
      map_verb_b (*ops_, verb_b::normal, verb_args, verb_arg);

      os << "b := " << search_b (*ops_).effect_string ();
      for (const char* o: verb_args) os << ' ' << o;
      for (const string& o: ops_->build_option ()) os << ' ' << o;
      os << '\n'
         << '\n'
         << "parallel := $(filter parallel=%,$(DEB_BUILD_OPTIONS))" << '\n'
         << "ifneq ($(parallel),)"                                  << '\n'
         << "  parallel := $(patsubst parallel=%,%,$(parallel))"    << '\n'
         << "  ifeq ($(parallel),1)"                                << '\n'
         << "    b += --serial-stop"                                << '\n'
         << "  else"                                                << '\n'
         << "    b += --jobs=$(parallel)"                           << '\n'
         << "  endif"                                               << '\n'
         << "endif"                                                 << '\n'
         << '\n';

      // Configuration variables.
      //
      // Note: we need to quote values that contain `<>`, `[]`, since they
      // will be passed through shell. For simplicity, let's just quote
      // everything.
      //
      os << "config := config.install.chroot='$(DESTDIR)/'"   << '\n'
         << "config += config.install.sudo='[null]'"          << '\n';

      // If this is a C-based language, add rpath for private installation.
      //
      if (priv && (lang_c || lang_cxx || lang_cc))
        os << "config += config.bin.rpath='/usr/lib/$(DEB_HOST_MULTIARCH)/"
           <<            pn << "/'" << '\n';

      // Add build flags.
      //
      if (ops_->debian_buildflags () != "ignore")
      {
        const string& m (ops_->debian_buildflags ());

        string o (m == "assign"  ? "="  :
                  m == "append"  ? "+=" :
                  m == "prepend" ? "=+" : "");

        if (o.empty ())
          fail << "unknown --debian-buildflags option value '" << m << "'";

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
        // overridden.
        //
        if (o == "=" && (lang_c || lang_cxx || lang_cc))
        {
          os << "config += config.cc.poptions='[null]'" << '\n'
             << "config += config.cc.coptions='[null]'" << '\n'
             << "config += config.cc.loptions='[null]'" << '\n';
        }

        if (lang_c || lang_cc)
        {
          // @@ TODO: OBJCFLAGS (we currently don't have separate options).
          //          Also see -ffile-prefix-map fixup above.

          os << "config += config.c.poptions" << o << "'$(CPPFLAGS)'" << '\n'
             << "config += config.c.coptions" << o << "'$(CFLAGS)'"   << '\n'
             << "config += config.c.loptions" << o << "'$(LDFLAGS)'"  << '\n';
        }

        if (lang_cxx || lang_cc)
        {
          // @@ TODO: OBJCXXFLAGS (we currently don't have separate options).
          //          Also see -ffile-prefix-map fixup above.

          os << "config += config.cxx.poptions" << o << "'$(CPPFLAGS)'" << '\n'
             << "config += config.cxx.coptions" << o << "'$(CXXFLAGS)'" << '\n'
             << "config += config.cxx.loptions" << o << "'$(LDFLAGS)'"  << '\n';
        }

        // @@ TODO: ASFLAGS (when we have assembler support).
      }

      // Keep last to allow user-specified configuration variables to override
      // anything.
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
              os << "config += " << string (c, 0, p) << '\''
                 << string (c, p) << "'\n";
              continue;
            }
          }
        }

        os << "config += " << c << '\n';
      }

      os << '\n';

      // List of packages we need to install.
      //
      for (auto b (pkgs.begin ()), i (b); i != pkgs.end (); ++i)
      {
        os << "packages" << (i == b ? " := " : " += ")
           << i->out_root.representation () << '\n';
      }
      os << '\n';

      // Default to the dh command sequencer.
      //
      // Note that passing --buildsystem=none doesn't seem to make any
      // difference (other than add some noise).
      //
      os << "%:\n"
         << '\t' << "dh $@" << '\n'
         << '\n';

      // Override dh_auto_configure.
      //
      os << "# Everything is already configured.\n"
         << "#\n"
         << "override_dh_auto_configure:\n"
         << '\n';

      // Override dh_auto_build.
      //
      os << "override_dh_auto_build:\n"
         << '\t' << "$b $(config) update-for-install: $(packages)" << '\n'
         << '\n';

      // Override dh_auto_test.
      //
      // Note that running tests after update-for-install may cause rebuild
      // (e.g., relinking without rpath, etc) before tests and again before
      // install. So doesn't seem worth the trouble.
      //
      os << "# Assume any testing has already been done.\n"
         << "#\n"
         << "override_dh_auto_test:\n"
         << '\n';

      // Override dh_auto_install.
      //
      os << "override_dh_auto_install:\n"
         << '\t' << "$b $(config) '!config.install.scope=" << scope << "' "
         <<         "install: $(packages)" << '\n'
         << '\n';

      // Override dh_auto_clean.
      //
      os << "# This is not a real source directory so nothing to clean.\n"
         << "#\n"
         << "override_dh_auto_clean:\n"
         << '\n';

      // Override dh_shlibdeps.
      //
      // Failed that we get a warning about calculated ${shlibs:Depends} being
      // unused.
      //
      // Note that there is also dh_makeshlibs which is invoked just before
      // but we shouldn't override it because (quoting its man page): "It will
      // also ensure that ldconfig is invoked during install and removal when
      // it finds shared libraries."
      //
      os << "# Disable dh_shlibdeps since we don't use ${shlibs:Depends}.\n"
         << "#\n"
         << "override_dh_shlibdeps:\n"
         << '\n';

      os.close ();
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << rules << ": " << e;
    }

    // Generate the dh_install (.install) config files for each package in
    // order to sort out which files belong where.
    //
    // For documentation of the config file format see debhelper(1) and
    // dh_install(1). But the summary is:
    //
    //   - Supports only simple wildcards (?, *, [...]; no recursive/**).
    //   - But can install whole directories recursively.
    //   - An entry that doesn't match anything is an error (say, /usr/sbin/*).
    //   - Supports variable substitutions (${...}; since compat level 13).
    //
    // Keep in mind that wherever there is <project> in the config.install.*
    // variable, we can end up with multiple different directories (bundled
    // packages).
    //
    path main_install;
    path dev_install;
    path doc_install;
    path dbg_install;
    path common_install;

    const path* cur_install (nullptr); // File being opened/written to.
    try
    {
      ofdstream main_os;
      ofdstream dev_os;
      ofdstream doc_os;
      ofdstream dbg_os;
      ofdstream com_os;

      pair<path&, ofdstream&> main (main_install,   main_os);
      pair<path&, ofdstream&> dev  (dev_install,    dev_os);
      pair<path&, ofdstream&> doc  (doc_install,    doc_os);
      pair<path&, ofdstream&> dbg  (dbg_install,    dbg_os);
      pair<path&, ofdstream&> com  (common_install, com_os);

      auto open = [&deb, &cur_install] (pair<path&, ofdstream&>& os,
                                        const string& n)
      {
        if (!n.empty ())
        {
          cur_install = &(os.first = deb / (n + ".install"));
          os.second.open (os.first);
        }
      };

      open (main, gen_main ? st.main : empty_string);
      open (dev, st.dev);
      open (doc, st.doc);
      open (dbg, st.dbg);
      open (com, st.common);

      auto is_open = [] (pair<path&, ofdstream&>& os)
      {
        return os.second.is_open ();
      };

      auto add = [&cur_install] (pair<path&, ofdstream&>& os, const path& p)
      {
        // Strip root.
        //
        string s (p.leaf (p.root_directory ()).string ());

        // Replace () with {}.
        //
        for (char& c: s)
        {
          if (c == '(') c = '{';
          if (c == ')') c = '}';
        }

        cur_install = &os.first;
        os.second << s  << '\n';
      };

      // The main package contains everything that doesn't go to another
      // packages.
      //
      if (gen_main)
      {
        if (ies.contains_sub (bindir))  add (main,  bindir / "*");
        if (ies.contains_sub (sbindir)) add (main, sbindir / "*");

        // This could potentially go to -common but it could also be target-
        // specific, who knows. So let's keep it in main for now.
        //
        if (ies.contains_sub (etcdir)) add (main, etcdir / "*");
      }

      if (!is_open (dev))
      {
        assert (gen_main); // Shouldn't be here otherwise.

        if (ies.contains_sub (incdir))     add (main, incdir / "*");
        if (ies.contains_sub (incarchdir)) add (main, incarchdir / "*");
        if (ies.contains_sub (libdir))     add (main, libdir / "*");
      }
      else
      {
        if (ies.contains_sub (incdir))     add (dev, incdir / "*");
        if (ies.contains_sub (incarchdir)) add (dev, incarchdir / "*");

        // Ok, time for things to get hairy: we need to split the contents
        // of lib/ into the main and -dev packages. The -dev package should
        // contain three things:
        //
        // 1. Static libraries (.a).
        // 2. Non-versioned shared library symlinks (.so).
        // 3. Contents of the pkgconfig/ subdirectory.
        //
        // Everything else should go into the main package. In particular, we
        // assume any subdirectories other than pkgconfig/ are the libexec
        // stuff or similar.
        //
        // The (2) case (shared library) is tricky. Here we can have three
        // plausible arrangements:
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
        // package. Based on this, the criteria appears to be straightforward:
        // the extension is .so and it's a symlink. For good measure we also
        // check that there is the `lib` prefix (plugins, etc).
        //
        for (auto p (ies.find_sub (libdir)); p.first != p.second; )
        {
          const path& f (p.first->first);
          const installed_entry& ie ((p.first++)->second);

          path l (f.leaf (libdir));

          if (l.simple ())
          {
            assert (gen_main); // Shouldn't be here otherwise.

            string e (l.extension ());
            const string& n (l.string ());

            bool d (n.size () > 3 && n.compare (0, 3, "lib") == 0 &&
                    ((e == "a"                         ) ||
                     (e == "so" && ie.target != nullptr)));

            add (d ? dev : main, libdir / l);
          }
          else
          {
            // Let's keep things tidy and use a wildcard rather than listing
            // all the entries in subdirectories verbatim.
            //
            dir_path d (libdir / dir_path (*l.begin ()));

            // Can only be a subdirectory of pkgdir/ if the main package is
            // not being generated.
            //
            assert (d == pkgdir || gen_main);

            add (d == pkgdir ? dev : main, d / "*");

            // Skip all the other entries in this subdirectory (in the prefix
            // map they will all be in a contiguous range).
            //
            while (p.first != p.second && p.first->first.sub (d))
              ++p.first;
          }
        }
      }

      // We cannot just do usr/share/* since it will clash with doc/ and man/
      // below. So we have to list all the top-level entries in usr/share/
      // that are not doc/ or man/.
      //
      if (gen_main)
      {
        // Note: covers bfdir.
        //
        for (auto p (ies.find_sub (sharedir)); p.first != p.second; )
        {
          const path& f ((p.first++)->first);

          if (f.sub (docdir) || f.sub (mandir))
            continue;

          path l (f.leaf (sharedir));

          if (l.simple ())
            add (is_open (com) ? com : main, sharedir / l);
          else
          {
            // Let's keep things tidy and use a wildcard rather than listing
            // all the entries in subdirectories verbatim.
            //
            dir_path d (sharedir / dir_path (*l.begin ()));

            add (is_open (com) ? com : main, d / "*");

            // Skip all the other entries in this subdirectory (in the prefix
            // map they will all be in a contiguous range).
            //
            while (p.first != p.second && p.first->first.sub (d))
              ++p.first;
          }
        }
      }

      // Should we put the documentation into -common if there is no -doc?
      // While there doesn't seem to be anything explicit in the policy, there
      // are packages that do it this way (e.g., libao, libaudit). And the
      // same logic seems to apply to -dev (e.g., zlib).
      //
      {
        auto& os (is_open (doc) ? doc :
                  is_open (com) ? com :
                  is_open (dev) ? dev :
                  main);

        // We can only add doc files to the main or -common packages if we
        // generate the main package.
        //
        assert ((&os != &main && &os != &com) || gen_main);

        if (ies.contains_sub (docdir)) add (os, docdir / "*");
        if (ies.contains_sub (mandir)) add (os, mandir / "*");
      }

      // Close.
      //
      auto close = [&cur_install] (pair<path&, ofdstream&>& os)
      {
        if (os.second.is_open ())
        {
          cur_install = &os.first;
          os.second.close ();
        }
      };

      close (main);
      close (dev);
      close (doc);
      close (dbg);
      close (com);
    }
    catch (const io_error& e)
    {
      fail << "unable to write to " << *cur_install << ": " << e;
    }

    // Run dpkg-buildpackage.
    //
    // Note that there doesn't seem to be any way to control its verbosity or
    // progress.
    //
    // Note also that dpkg-buildpackage causes recompilation on every run by
    // changing the SOURCE_DATE_EPOCH environment variable (which we track for
    // changes since it affects GCC). Note that since we don't have this
    // SOURCE_DATE_EPOCH during dry-run caused by installed_entries(), there
    // would be a recompilation even if the value weren't changing.
    //
    cstrings args {
      "dpkg-buildpackage",
      "--build=binary",                // Only build binary packages.
      "--no-sign",                     // Do not sign anything.
      "--target-arch", arch.c_str ()};

    // Pass our --jobs value, if any.
    //
    string jobs_arg;
    if (size_t n = ops_->jobs_specified () ? ops_->jobs () : 0)
    {
      // Note: only accepts the --jobs=N form.
      //
      args.push_back ((jobs_arg = "--jobs=" + to_string (n)).c_str ());
    }

    // Pass any additional options specified by the user.
    //
    for (const string& o: ops_->debian_build_option ())
      args.push_back (o.c_str ());

    args.push_back (nullptr);

    if (ops_->debian_prepare_only ())
    {
      if (verb >= 1)
      {
        diag_record dr (text);

        dr << "prepared " << src <<
          text << "command line: ";

        print_process (dr, args);
      }

      return binary_files {};
    }

    try
    {
      process_path pp (process::path_search (args[0]));
      process_env pe (pp, src /* cwd */);

      // There is going to be quite a bit of diagnostics so print the command
      // line unless quiet.
      //
      if (verb >= 1)
        print_process (pe, args);

      // Redirect stdout to stderr since half of dpkg-buildpackage diagnostics
      // goes there. For good measure also redirect stdin to /dev/null to make
      // sure there are no prompts of any kind.
      //
      process pr (pp,
                  args,
                  -2 /* stdin */,
                  2  /* stdout */,
                  2  /* stderr */,
                  pe.cwd->string ().c_str (),
                  pe.vars);

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

    // Cleanup intermediate files unless requested not to.
    //
    if (!ops_->keep_output ())
    {
      rm_r (src);
    }

    // Collect and return the binary package paths.
    //
    binary_files r;
    r.system_version = st.system_version;

    auto add = [&out, &r] (const string& f,
                           const char* t,
                           const string& n,
                           bool opt = false)
    {
      path p (out / f);

      if (exists (p))
        r.push_back (binary_file {t, move (p), n});
      else if (!opt)
        fail << "expected output file " << f << " does not exist";
    };

    // The resulting .deb file names have the <name>_<version>_<arch>.deb
    // form. If the package is architecture-independent, then <arch> is the
    // special `all` value.
    //
    const string& v (st.system_version);
    const string& a (arch);

    if (gen_main)
      add (st.main + '_' + v + '_' + a + ".deb", "main.deb", st.main);

    if (!binless)
      add (st.main + "-dbgsym_" + v + '_' + a + ".deb",
           "dbgsym.deb",
           st.main + "-dbgsym",
           true);

    if (!st.dev.empty ())
      add (st.dev + '_' + v + '_' + a + ".deb", "dev.deb", st.dev);

    if (!st.doc.empty ())
      add (st.doc + '_' + v + "_all.deb", "doc.deb", st.doc);

    if (!st.common.empty ())
      add (st.common  + '_' + v + "_all.deb", "common.deb", st.common);

    // Besides the binary packages (.deb) we also get the .buildinfo and
    // .changes files, which could be useful. Note that their names are based
    // on the source package name.
    //
    add (pn.string () + '_' + v + '_' + a + ".buildinfo", "buildinfo", "");
    add (pn.string () + '_' + v + '_' + a + ".changes", "changes", "");

    return r;
  }
}
