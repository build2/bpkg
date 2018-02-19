// file      : bpkg/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/utility.hxx>

#include <iostream>     // cout, cin

#include <libbutl/process.mxx>
#include <libbutl/fdstream.mxx>

#include <bpkg/diagnostics.hxx>
#include <bpkg/common-options.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  const string empty_string;
  const path empty_path;
  const dir_path empty_dir_path;

  const dir_path bpkg_dir (".bpkg");
  const dir_path certs_dir (dir_path (bpkg_dir) /= "certificates");
  const dir_path repos_dir (dir_path (bpkg_dir) /= "repositories");

  const dir_path current_dir (".");

  dir_path temp_dir;

  auto_rmfile
  tmp_file (const string& p)
  {
    assert (!temp_dir.empty ());
    return auto_rmfile (temp_dir / path::traits::temp_name (p));
  }

  auto_rmdir
  tmp_dir (const string& p)
  {
    assert (!temp_dir.empty ());
    return auto_rmdir (temp_dir / dir_path (path::traits::temp_name (p)));
  }

  void
  init_tmp (const dir_path& cfg)
  {
    // Whether the configuration is required or optional depends on the
    // command so if the configuration directory does not exist or it is not a
    // bpkg configuration directory, we simply create tmp in a system one and
    // let the command complain if necessary.
    //
    dir_path d (cfg.empty () ||
                !exists (cfg / bpkg_dir, true /* ignore_error */)
                ? dir_path::temp_path ("bpkg")
                : cfg / bpkg_dir / dir_path ("tmp"));

    if (exists (d))
      rm_r (d, true /* dir_itself */, 1); // Verbose to avoid surprises.

    mk (d); // We shouldn't need mk_p().

    temp_dir = move (d);
  }

  void
  clean_tmp (bool ignore_error)
  {
    if (!temp_dir.empty ())
    {
      rm_r (temp_dir, true /* dir_itself */, 3, ignore_error);
      temp_dir.clear ();
    }
  }

  bool
  yn_prompt (const char* prompt, char def)
  {
    // Writing a robust Y/N prompt is more difficult than one would
    // expect...
    //
    string a;
    do
    {
      *diag_stream << prompt << ' ';

      // getline() will set the failbit if it failed to extract anything,
      // not even the delimiter and eofbit if it reached eof before seeing
      // the delimiter.
      //
      getline (cin, a);

      bool f (cin.fail ());
      bool e (cin.eof ());

      if (f || e)
        *diag_stream << endl; // Assume no delimiter (newline).

      if (f)
        fail << "unable to read y/n answer from STDOUT";

      if (a.empty () && def != '\0')
      {
        // Don't treat eof as the default answer. We need to see the
        // actual newline.
        //
        if (!e)
          a = def;
      }
    } while (a != "y" && a != "n");

    return a == "y";
  }

  bool
  exists (const path& f, bool ignore_error)
  {
    try
    {
      return file_exists (f, true /* follow_symlinks */, ignore_error);
    }
    catch (const system_error& e)
    {
      fail << "unable to stat path " << f << ": " << e << endf;
    }
  }

  bool
  exists (const dir_path& d, bool ignore_error)
  {
    try
    {
      return dir_exists (d, ignore_error);
    }
    catch (const system_error& e)
    {
      fail << "unable to stat path " << d << ": " << e << endf;
    }
  }

  bool
  empty (const dir_path& d)
  {
    try
    {
      return dir_empty (d);
    }
    catch (const system_error& e)
    {
      fail << "unable to scan directory " << d << ": " << e << endf;
    }
  }

  void
  mk (const dir_path& d)
  {
    if (verb >= 3)
      text << "mkdir " << d;

    try
    {
      try_mkdir (d);
    }
    catch (const system_error& e)
    {
      fail << "unable to create directory " << d << ": " << e;
    }
  }

  void
  mk_p (const dir_path& d)
  {
    if (verb >= 3)
      text << "mkdir -p " << d;

    try
    {
      try_mkdir_p (d);
    }
    catch (const system_error& e)
    {
      fail << "unable to create directory " << d << ": " << e;
    }
  }

  void
  rm (const path& f, uint16_t v)
  {
    if (verb >= v)
      text << "rm " << f;

    try
    {
      if (try_rmfile (f) == rmfile_status::not_exist)
        fail << "unable to remove file " << f << ": file does not exist";
    }
    catch (const system_error& e)
    {
      fail << "unable to remove file " << f << ": " << e;
    }
  }

  void
  rm_r (const dir_path& d, bool dir, uint16_t v, bool ignore_error)
  {
    if (verb >= v)
      text << (dir ? "rmdir -r " : "rm -r ") << (dir ? d : d / dir_path ("*"));

    try
    {
      rmdir_r (d, dir, ignore_error);
    }
    catch (const system_error& e)
    {
      fail << "unable to remove " << (dir ? "" : "contents of ")
           << "directory " << d << ": " << e;
    }
  }

  void
  mv (const dir_path& from, const dir_path& to)
  {
    if (verb >= 3)
      text << "mv " << from << " to " << to; // Prints trailing slashes.

    try
    {
      mvdir (from, to);
    }
    catch (const system_error& e)
    {
      fail << "unable to move directory " << from << " to " << to << ": " << e;
    }
  }

  fdpipe
  open_pipe ()
  {
    try
    {
      return fdopen_pipe ();
    }
    catch (const io_error& e)
    {
      fail << "unable to open pipe: " << e << endf;
    }
  }

  auto_fd
  open_dev_null ()
  {
    try
    {
      return fdnull ();
    }
    catch (const io_error& e)
    {
      fail << "unable to open null device: " << e << endf;
    }
  }

  dir_path exec_dir;

  void
  run (const char* args[], const dir_path& fallback)
  {
    try
    {
      process_path pp (process::path_search (args[0], fallback));

      if (verb >= 2)
        print_process (args);

      process pr (pp, args);

      if (pr.wait ())
        return;

      assert (pr.exit);
      const process_exit& pe (*pr.exit);

      if (pe.normal ())
        throw failed (); // Assume the child issued diagnostics.

      diag_record dr (fail);
      print_process (dr, args);
      dr << " " << pe;
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  const char*
  name_b (const common_options& co)
  {
    return co.build_specified ()
      ? co.build ().string ().c_str ()
      : "b" BPKG_EXE_SUFFIX;
  }

  void
  run_b (const common_options& co,
         const dir_path& c,
         const string& bspec,
         bool quiet,
         const strings& pvars,
         const strings& cvars)
  {
    cstrings args {name_b (co)};

    // Map verbosity level. If we are running quiet or at level 1,
    // then run build2 quiet. Otherwise, run it at the same level
    // as us.
    //
    string vl;
    if (verb <= (quiet ? 1 : 0))
      args.push_back ("-q");
    else if (verb == 2)
      args.push_back ("-v");
    else if (verb > 2)
    {
      vl = to_string (verb);
      args.push_back ("--verbose");
      args.push_back (vl.c_str ());
    }

    // Add user options.
    //
    for (const string& o: co.build_option ())
      args.push_back (o.c_str ());

    // Add config vars.
    //
    strings storage;
    storage.reserve (cvars.size ());
    for (const string& v: cvars)
    {
      // Don't scope-qualify global variables.
      //
      if (v[0] != '!')
      {
        // Use path representation to get canonical trailing slash.
        //
        storage.push_back (c.representation () + ':' + v);
        args.push_back (storage.back ().c_str ());
      }
      else
        args.push_back (v.c_str ());
    }

    for (const string& v: pvars)
      args.push_back (v.c_str ());

    // Add buildspec.
    //
    args.push_back (bspec.c_str ());

    args.push_back (nullptr);

    // Use our executable directory as a fallback search since normally the
    // entire toolchain is installed into one directory. This way, for
    // example, if we installed into /opt/build2 and run bpkg with absolute
    // path (and without PATH), then bpkg will be able to find "its" b.
    //
    run (args, exec_dir);
  }
}
