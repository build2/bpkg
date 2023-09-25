// file      : bpkg/system-package-manager-archive.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager-archive.hxx>

#include <map>

#include <bpkg/diagnostics.hxx>

#include <bpkg/pkg-bindist-options.hxx>

using namespace butl;

namespace bpkg
{
  system_package_manager_archive::
  system_package_manager_archive (bpkg::os_release&& osr,
                                  const target_triplet& h,
                                  string a,
                                  optional<bool> progress,
                                  const pkg_bindist_options* options)
      : system_package_manager (move (osr), h, "", progress), ops (options)
  {
    if (!a.empty ())
    {
      assert (ops != nullptr);

      try
      {
        target = target_triplet (a);
      }
      catch (const invalid_argument& e)
      {
        fail << "invalid --architecture target triplet value '" << a << "': "
             << e;
      }

      if (!ops->os_release_id_specified ())
        fail << "--architecture requires explict --os-release-id";

      if (!ops->archive_install_root_specified () &&
          !ops->archive_install_config ())
        fail << "--architecture requires explict --archive-install-root";
    }
    else
      target = host;

    arch = target.string (); // Set since queried (e.g., JSON value).
  }

  // env --chdir=<root> tar|zip ... <base>.<ext> <base>
  //
  // Return the archive file path.
  //
  static path
  archive (const dir_path& root,
           const string& base,
           const string& e /* ext */)
  {
    // NOTE: similar code in build2 (libbuild2/dist/operation.cxx).

    path an (base + '.' + e);
    path ap (root / an);

    // Use zip for .zip archives. Also recognize and handle a few well-known
    // tar.xx cases (in case tar doesn't support -a or has other issues like
    // MSYS). Everything else goes to tar in the auto-compress mode (-a).
    //
    // Note also that we pass the archive path as name (an) instead of path
    // (ap) since we are running from the root directory (see below).
    //
    cstrings args;

    // Separate compressor (gzip, xz, etc) state.
    //
    size_t i (0);        // Command line start or 0 if not used.
    auto_rmfile out_rm;  // Output file cleanup (must come first).
    auto_fd out_fd;      // Output file.

    if (e == "zip")
    {
      // On Windows we use libarchive's bsdtar (zip is an MSYS executable).
      //
      // While not explicitly stated, the compression-level option works
      // for zip archives.
      //
#ifdef _WIN32
      args = {"bsdtar",
              "-a", // -a with the .zip extension seems to be the only way.
              "--options=compression-level=9",
              "-cf", an.string ().c_str (),
              base.c_str (),
              nullptr};
#else
      args = {"zip",
              "-9",
              "-rq", an.string ().c_str (),
              base.c_str (),
              nullptr};
#endif
    }
    else
    {
      // On Windows we use libarchive's bsdtar with auto-compression (tar
      // itself and quite a few compressors are MSYS executables).
      //
      // OpenBSD tar does not support --format but it appear ustar is the
      // default (while this is not said explicitly in tar(1), it is said in
      // pax(1) and confirmed on the mailing list). Nor does it support -a, at
      // least as of 7.1 but we will let this play out naturally, in case this
      // support gets added.
      //
      // Note also that in the future we may switch to libarchive in order to
      // generate reproducible archives.
      //
      const char* l (nullptr); // Compression level (option).

#ifdef _WIN32
      args = {"bsdtar", "--format", "ustar"};

      if (e == "tar.gz" || e == "tar.xz")
        l = "--options=compression-level=9";
#else
      args = {"tar"
#ifndef __OpenBSD__
              , "--format", "ustar"
#endif
      };

      // For gzip it's a good idea to use -9 by default. While for xz, -9 is
      // not recommended as the default due to memory requirements, in our
      // case (large binary archives on development machines), this is
      // unlikely to be an issue.
      //
      // Note also that the compression level can be altered via the GZIP
      // (GZIP_OPT also seems to work) and XZ_OPT environment variables,
      // respectively.
      //
      const char* c (nullptr);

      if      (e == "tar.gz")  { c = "gzip";  l = "-9"; }
      else if (e == "tar.xz")
      {
        // At least as of Mac OS 13 and Xcode 15, there is no standalone xz
        // utility but tar seem to be capable of producing .tar.xz.
        //
#ifdef __APPLE__
        l = "--options=compression-level=9";
#else
        c = "xz";    l = "-9";
#endif
      }

      if (c != nullptr)
      {
        args.push_back ("-cf");
        args.push_back ("-");
        args.push_back (base.c_str ());
        args.push_back (nullptr); i = args.size ();
        args.push_back (c);
        if (l != nullptr)
          args.push_back (l);
        args.push_back (nullptr);
        args.push_back (nullptr); // Pipe end.

        try
        {
          out_fd = fdopen (ap,
                           fdopen_mode::out      | fdopen_mode::binary |
                           fdopen_mode::truncate | fdopen_mode::create);
          out_rm = auto_rmfile (ap);
        }
        catch (const io_error& e)
        {
          fail << "unable to open " << ap << ": " << e;
        }
      }
      else
#endif
      {
        if (e != "tar")
        {
          args.push_back ("-a");
          if (l != nullptr)
            args.push_back (l);
        }

        args.push_back ("-cf");
        args.push_back (an.string ().c_str ());
        args.push_back (base.c_str ());
        args.push_back (nullptr);
      }
    }

    size_t what (0); // Failed program name index in args.
    try
    {
      process_path app; // Archiver path.
      process_path cpp; // Compressor path.

      app = process::path_search (args[what = 0]);

      if (i != 0)
        cpp = process::path_search (args[what = i]);

      // Change the archiver's working directory to root.
      //
      process_env ape (app, root);

      // Note: print the command line unless quiet similar to other package
      // manager implementations.
      //
      if (verb >= 1)
        print_process (ape, args);

      what = 0;
      process apr (app,
                   args.data (), // No auto-pipe.
                   0                  /* stdin */,
                   (i != 0 ? -1 : 1)  /* stdout */,
                   2                  /* stderr */,
                   ape.cwd->string ().c_str (),
                   ape.vars);

      // Start the compressor if required.
      //
      process cpr;
      if (i != 0)
      {
        what = i;
        cpr = process (cpp,
                       args.data () + i,
                       apr.in_ofd.get () /* stdin  */,
                       out_fd.get ()     /* stdout */,
                       2                 /* stderr */);

        cpr.in_ofd.reset (); // Close the archiver's stdout on our side.
      }

      // Delay throwing until we diagnose both ends of the pipe.
      //
      bool fail (false);

      what = 0;
      if (!apr.wait ())
      {
        diag_record dr (error);
        dr << args[0] << " exited with non-zero code";

        if (verb == 0)
        {
          info << "command line: ";
          print_process (dr, ape, args.data ());
        }

        fail = true;
      }

      if (i != 0)
      {
        what = i;
        if (!cpr.wait ())
        {
          diag_record dr (error);
          dr << args[i] << " exited with non-zero code";

          if (verb == 0)
          {
            info << "command line: ";
            print_process (dr, args.data () + i);
          }

          fail = true;
        }
      }

      if (fail)
        throw failed ();
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[what] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }

    out_rm.cancel ();
    return ap;
  }

  // NOTE: THE BELOW DESCRIPTION IS ALSO REWORDED IN BPKG-PKG-BINDIST(1).
  //
  // The overall plan is to invoke the build system and install all the
  // packages directly from their bpkg locations into the binary package
  // directory as a chroot. Then tar/zip this directory to produce one or more
  // binary package archives.
  //
  auto system_package_manager_archive::
  generate (const packages& pkgs,
            const packages& deps,
            const strings& vars,
            const dir_path& /*cfg_dir*/,
            const package_manifest& pm,
            const string& pt,
            const small_vector<language, 1>& langs,
            optional<bool> recursive_full,
            bool first) -> binary_files
  {
    tracer trace ("system_package_manager_archive::generate");

    assert (!langs.empty ()); // Should be effective.

    // We require explicit output root.
    //
    if (!ops->output_root_specified ())
      fail << "output root directory must be specified explicitly with "
           << "--output-root|-o";

    const dir_path& out (ops->output_root ()); // Cannot be empty.

    const shared_ptr<selected_package>& sp (pkgs.front ().selected);
    const package_name& pn (sp->name);
    const version& pv (sp->version);

    // Use version without iteration in paths, etc.
    //
    string pvs (pv.string (false /* ignore_revision */,
                           true  /* ignore_iteration */));

    bool lib (pt == "lib");
    bool priv (ops->private_ ()); // Private installation.

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

    bool lang_c   (lang ("c"));
    bool lang_cxx (lang ("c++"));
    bool lang_cc  (lang ("cc"));

    if (verb >= 3)
    {
      auto print_status = [] (diag_record& dr, const selected_package& p)
      {
        dr << (p.substate == package_substate::system ? "sys:" : "")
           << p.name << ' ' << p.version;
      };

      {
        diag_record dr (trace);
        dr << "package: " ;
        print_status (dr, *sp);
      }

      for (const package& p: deps)
      {
        diag_record dr (trace);
        dr << "dependency: ";
        print_status (dr, *p.selected);
      }
    }

    // Should we override config.install.* or just use whatever configured
    // (sans the root)? While using whatever configure seemed like a good idea
    // at first, it's also a good idea to have the ability to tweak the
    // installation directory structure on the per-platform basis (like, say,
    // lib/libexec split or pkgconfig/ location on FreeBSD; in a sense, the
    // user may choose to install to /usr and it would be good if things ended
    // up in the expected places -- this is still a @@ TODO).
    //
    // So unless instructed otherwise with --archive-install-config, we
    // override every config.install.* variable in order not to pick anything
    // configured. Note that we add some more in the command line below.
    //
    // We make use of the <project> substitution since in the recursive mode
    // we may be installing multiple projects. Note that the <private>
    // directory component is automatically removed if this functionality is
    // not enabled.
    //
    bool ovr_install (!ops->archive_install_config ());

    strings config;
    {
      const string& c (target.class_);

      dir_path root;
      if (ops->archive_install_root_specified ())
      {
        // If specified, we override it even with --archive-install-config.
        //
        root = ops->archive_install_root (); // Cannot be empty.
      }
      else if (ovr_install)
      {
        if (c == "windows")
        {
          // Using C:\<project>\ looks like the best we can do (if the
          // installation is not relocatable, at least related packages will
          // be grouped together).
          //
          root = dir_path ("C:\\" + pm.effective_project ().string ());
        }
        else
          root = dir_path ("/usr/local");
      }

      auto add = [&config] (auto&& v)
      {
        config.push_back (string ("config.install.") + v);
      };

      if (!root.empty ())
        add ("root='" + root.representation () + '\'');

      if (ovr_install)
      {
        add ("data_root=root/");
        add ("exec_root=root/");

        add ("bin=exec_root/bin/");
        add ("sbin=exec_root/sbin/");

        add ("lib=exec_root/lib/<private>/");
        add ("libexec=exec_root/libexec/<private>/<project>/");
        add ("pkgconfig=lib/pkgconfig/");

        add ("etc=data_root/etc/");
        add ("include=data_root/include/<private>/");
        add ("include_arch=include/");
        add ("share=data_root/share/");
        add ("data=share/<private>/<project>/");
        add ("buildfile=share/build2/export/<project>/");

        add ("doc=share/doc/<private>/<project>/");
        add ("legal=doc/");
        add ("man=share/man/");
        add ("man1=man/man1/");
        add ("man2=man/man2/");
        add ("man3=man/man3/");
        add ("man4=man/man4/");
        add ("man5=man/man5/");
        add ("man6=man/man6/");
        add ("man7=man/man7/");
        add ("man8=man/man8/");

        add ("private=" + (priv ? pn.string () : "[null]"));

        // If this is a C-based language, add rpath for private installation,
        // unless targeting Windows.
        //
        if (priv && (lang_c || lang_cxx || lang_cc) && c != "windows")
        {
          dir_path l ((dir_path (root) /= "lib") /= pn.string ());
          config.push_back ("config.bin.rpath='" + l.representation () + '\'');
        }
      }
    }

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

    // The plan is to create the archive directory (with the same name as the
    // archive base; we call it "destination directory") inside the output
    // directory and then tar/zip it up placing the resulting archives next to
    // it.
    //
    // Let's require clean output directory to keep things simple.
    //
    // Also, by default, we are going to keep all the intermediate files on
    // failure for troubleshooting.
    //
    if (first && exists (out) && !empty (out))
    {
      if (!ops->wipe_output ())
        fail << "output root directory " << out << " is not empty" <<
          info << "use --wipe-output to clean it up but be careful";

      rm_r (out, false);
    }

    // NOTE: THE BELOW DESCRIPTION IS ALSO REWORDED IN BPKG-PKG-BINDIST(1).
    //
    // Our archive directory/file base have the following form:
    //
    // <package>-<version>-<build_metadata>
    //
    // Where <build_metadata> in turn has the following form (unless overriden
    // with --archive-build-mata):
    //
    // <cpu>-<os>[-<langrt>...]
    //
    // For example:
    //
    // hello-1.2.3-x86_64-windows10
    // libhello-1.2.3-x86_64-windows10-msvc17.4
    // libhello-1.2.3-x86_64-debian11-gcc12-rust1.62
    //
    bool md_s (ops->archive_build_meta_specified ());
    const string& md (ops->archive_build_meta ());

    bool md_f (false);
    bool md_b (false);
    if (md_s && !md.empty ())
    {
      md_f = md.front () == '+';
      md_b = md.back () == '+';

      if (md_f && md_b) // Note: covers just `+`.
        fail << "invalid build metadata '" << md << "'";
    }

    vector<reference_wrapper<const pair<const string, string>>> langrt;
    if (!md_s || md_f || md_b)
    {
      // First collect the interface languages and then add implementation.
      // This way if different languages map to the same runtimes (e.g., C and
      // C++ mapped to gcc12), then we will always prefer the interface
      // version over the implementation (which could be different, for
      // example, libstdc++6 vs libstdc++-12-dev; but it's not clear how this
      // will be specified, won't they end up with different names as opposed
      // to gcc6 and gcc12 -- still fuzzy/unclear).
      //
      // @@ We will need to split id and version to be able to pick the
      //    highest version.
      //
      // @@ Maybe we should just do "soft" version like in <distribution>?
      //
      // Note that we allow multiple values for the same language to support
      // cases like --archive-lang cc=gcc12 --archive-lang cc=g++12. But
      // we treat an empty value as a request to clear all the previous
      // entries.
      //

      auto find = [] (const std::multimap<string, string>& m, const string& n)
      {
        auto p (m.equal_range (n));

        if (p.first == p.second)
        {
          // If no mapping for c/c++, fallback to cc.
          //
          if (n == "c" || n == "c++")
            p = m.equal_range ("cc");
        }

        return p;
      };

      // We don't want to clear entries specified with --*-lang with an empty
      // value specified with --*-lang-impl.
      //
      size_t clear_limit (0);

      auto add = [&langrt, &clear_limit] (const pair<const string, string>& p)
      {
        // Suppress duplicates.
        //
        if (!p.second.empty ())
        {
          if (find_if (langrt.begin (), langrt.end (),
                       [&p] (const pair<const string, string>& x)
                       {
                         // @@ TODO: keep highest version.
                         return p.second == x.second;
                       }) == langrt.end ())
          {
            langrt.push_back (p);
          }
        }
        else if (clear_limit != langrt.size ())
        {
          for (auto i (langrt.begin () + clear_limit); i != langrt.end (); )
          {
            if (i->get ().first == p.first)
              i = langrt.erase (i);
            else
              ++i;
          }
        }
      };

      auto& implm (ops->archive_lang_impl ());

      // The interface/implementation distinction is only relevant to
      // libraries. For everything else we treat all the languages as
      // implementation.
      //
      if (lib)
      {
        auto& intfm (ops->archive_lang ());

        for (const language& l: langs)
        {
          if (l.impl)
            continue;

          auto p (find (intfm, l.name));

          if (p.first == p.second)
            p = find (implm, l.name);

          if (p.first == p.second)
            fail << "no runtime mapping for language " << l.name <<
              info << "consider specifying with --archive-lang[-impl]" <<
              info << "or alternatively specify --archive-build-meta";

          for (auto i (p.first); i != p.second; ++i)
            add (*i);
        }

        clear_limit = langrt.size ();
      }

      for (const language& l: langs)
      {
        if (lib && !l.impl)
          continue;

        auto p (find (implm, l.name));

        if (p.first == p.second)
          continue; // Unimportant.

        for (auto i (p.first); i != p.second; ++i)
          add (*i);
      }
    }

    // If there is no split, reduce to empty key and empty filter.
    //
    binary_files r;
    for (const pair<const string, string>& kf:
           ops->archive_split_specified ()
           ? ops->archive_split ()
           : std::map<string, string> {{string (), string ()}})
    {
      string sys_name (pn.string ());

      if (!kf.first.empty ())
        sys_name += '-' + kf.first;

      string base (sys_name);

      base += '-' + pvs;

      if (md_s && !(md_f || md_b))
      {
        if (!md.empty ())
          base += '-' + md;
      }
      else
      {
        if (md_b)
        {
          base += '-';
          base.append (md, 0, md.size () - 1);
        }

        if (!ops->archive_no_cpu ())
          base += '-' + target.cpu;

        if (!ops->archive_no_os ())
          base += '-' + os_release.name_id + os_release.version_id;

        for (const pair<const string, string>& p: langrt)
          base += '-' + p.second;

        if (md_f)
        {
          base += '-';
          base.append (md, 1, md.size () - 1);
        }
      }

      dir_path dst (out / dir_path (base));
      mk_p (dst);

      // Install.
      //
      // In a sense, this is a special version of pkg-install.
      //
      {
        strings dirs;
        for (const package& p: pkgs)
          dirs.push_back (p.out_root.representation ());

        string filter;
        if (!kf.second.empty ())
          filter = "config.install.filter=" + kf.second;

        run_b (*ops,
               verb_b::normal,
               (ops->jobs_specified ()
                ? strings ({"--jobs", to_string (ops->jobs ())})
                : strings ()),
               "config.install.chroot='" + dst.representation () + '\'',
               (ovr_install ? "config.install.sudo=[null]" : nullptr),
               (!filter.empty () ? filter.c_str () : nullptr),
               config,
               "!config.install.scope=" + scope,
               "install:",
               dirs);

        // @@ TODO: call install.json? Or manifest-install.json. Place in
        //    data/ (would need support in build2 to use install.* values)?
        //
#if 0
        args.push_back ("!config.install.manifest=-");
#endif
      }

      if (ops->archive_prepare_only ())
      {
        if (verb >= 1)
          text << "prepared " << dst;

        continue;
      }

      // Create the archive.
      //
      // Should the default archive type be based on host or target? I guess
      // that depends on where the result will be unpacked, and it feels like
      // target is more likely.
      //
      // @@ What about the ownerhip of the resulting file in the archive?
      //    We don't do anything for source archives, not sure why we should
      //    do something here.
      //
      for (string t: (ops->archive_type_specified ()
                      ? ops->archive_type ()
                      : strings {target.class_ == "windows" ? "zip" : "tar.xz"}))
      {
        // Help the user out if the extension is specified with the leading
        // dot.
        //
        if (t.size () > 1 && t.front () == '.')
          t.erase (0, 1);

        path f (archive (out, base, t));

        // Using archive type as file type seems appropriate. Add key before
        // the archive type, if any.
        //
        if (!kf.first.empty ())
          t = kf.first + '.' + t;

        r.push_back (binary_file {move (t), move (f), sys_name});
      }

      // Cleanup intermediate files unless requested not to.
      //
      if (!ops->keep_output ())
      {
        rm_r (dst);
      }
    }

    return r;
  }

  optional<const system_package_status*> system_package_manager_archive::
  status (const package_name&, const available_packages*)
  {
    assert (false);
    return nullopt;
  }

  void system_package_manager_archive::
  install (const vector<package_name>&)
  {
    assert (false);
  }
}
