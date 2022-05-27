// file      : bpkg/archive.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/archive.hxx>

#include <bpkg/utility.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;

namespace bpkg
{
  dir_path
  package_dir (const path& a)
  {
    // Strip the top-level extension and, as a special case, if the second-
    // level extension is .tar, strip that as well (e.g., .tar.bz2).
    //
    path d (a.leaf ().base ());
    if (d.extension () == "tar")
      d = d.base ();
    return path_cast<dir_path> (d);
  }

#ifdef _WIN32
  static inline bool
  bsdtar (const char* p)
  {
    const char* l (path::traits_type::find_leaf (p));
    return l != nullptr && icasecmp (l, "bsdtar", 6) == 0;
  }
#endif

  // Only the extract ('x') and list ('t') operations are supported.
  //
  static pair<cstrings, size_t>
  start (const common_options& co, char op, const path& a)
  {
    assert (op == 'x' || op == 't');

    cstrings args;

    // On Windows we default to libarchive's bsdtar with auto-decompression
    // (though there is also bsdcat which we could have used).
    //
    // OpenBSD tar does not support -O|--to-stdout and so far the best
    // solution seems to require bsdtar (libarchive) or gtar (GNU tar).
    //
    const char* tar;

    if (co.tar_specified ())
      tar = co.tar ().string ().c_str ();
    else
    {
#ifdef _WIN32
      tar = "bsdtar";
#elif defined(__OpenBSD__)
      // A bit wasteful to do this every time (and throw away the result).
      // Oh, well, the user can always "optimize" this away by passing
      // explicit --tar.
      //
      if (!process::try_path_search ("bsdtar", true).empty ())
        tar = "bsdtar";
      else if (!process::try_path_search ("gtar", true).empty ())
        tar = "gtar";
      else
        fail << "bsdtar or gtar required on OpenBSD for -O|--to-stdout support"
             << endf;
#else
      tar = "tar";
#endif
    }

    // See if we need to decompress.
    //
    {
      const char* d (nullptr);
      string e (a.extension ());

      if      (e == "gz")    d = "gzip";
      else if (e == "bzip2") d = "bzip2";
      else if (e == "xz")    d = "xz";
      else if (e != "tar")   fail << "unknown compression method in " << a;

#ifdef _WIN32
      if (!bsdtar (tar))
#endif
        args.push_back (d);
    }

    size_t i (0); // The tar command line start.
    if (!args.empty ())
    {
      args.push_back ("-dc");
      args.push_back (a.string ().c_str ());
      args.push_back (nullptr);
      i = args.size ();
    }

    args.push_back (tar);

    // Add user's extra options.
    //
    for (const string& o: co.tar_option ())
      args.push_back (o.c_str ());

    // An archive name that has a colon in it specifies a file or device on a
    // remote machine. That makes it impossible to use absolute Windows paths
    // unless we add the --force-local option. Note that BSD tar doesn't
    // support this option but appears to do the right thing on Windows.
    //
#ifdef _WIN32
    if (!bsdtar (tar))
      args.push_back ("--force-local");
#endif

    args.push_back (op == 'x' ? "-xf" : "-tf");
    args.push_back (i == 0 ? a.string ().c_str () : "-");

    return make_pair (move (args), i);
  }

  pair<process, process>
  start_extract (const common_options& co, const path& a, const dir_path& d)
  {
    pair<cstrings, size_t> args_i (start (co, 'x', a));
    cstrings& args (args_i.first);
    size_t i (args_i.second);

    // -C/--directory -- change to directory.
    //
    args.push_back ("-C");

#ifdef _WIN32
    // MSYS GNU tar misinterprets -C option's absolute paths on Windows,
    // unless only forward slashes are used as directory separators:
    //
    // tar -C c:\a\cfg --force-local -xf c:\a\cfg\libbutl-0.7.0.tar.gz
    // tar: c\:\a\\cfg: Cannot open: No such file or directory
    // tar: Error is not recoverable: exiting now
    //
    string cwd;
    if (!bsdtar (args[i]))
    {
      cwd = d.string ();
      replace (cwd.begin (), cwd.end (), '\\', '/');
      args.push_back (cwd.c_str ());
    }
    else
#endif
      args.push_back (d.string ().c_str ());

    args.push_back (nullptr);
    args.push_back (nullptr); // Pipe end.

    size_t what;
    try
    {
      process_path dpp;
      process_path tpp;

      process dpr;
      process tpr;

      if (i != 0)
        dpp = process::path_search (args[what = 0]);

      tpp = process::path_search (args[what = i]);

      if (verb >= 2)
        print_process (args);

      if (i != 0)
      {
        dpr = process (dpp, &args[what = 0], 0, -1);
        tpr = process (tpp, &args[what = i], dpr);
      }
      else
      {
        dpr = process (process_exit (0)); // Successfully exited.
        tpr = process (tpp, &args[what = 0]);
      }

      return make_pair (move (dpr), move (tpr));
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[what] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  // Only the extract ('x') and list ('t') operations are supported.
  //
  static pair<process, process>
  start (const common_options& co,
         char op,
         const path& a,
         const cstrings& tar_args,
         bool diag)
  {
    pair<cstrings, size_t> args_i (start (co, op, a));
    cstrings& args (args_i.first);
    size_t i (args_i.second);

    args.insert (args.end (), tar_args.begin (), tar_args.end ());

    args.push_back (nullptr);
    args.push_back (nullptr); // Pipe end.

    size_t what;
    try
    {
      process_path dpp;
      process_path tpp;

      process dpr;
      process tpr;

      if (i != 0)
        dpp = process::path_search (args[what = 0]);

      tpp = process::path_search (args[what = i]);

      if (verb >= 2)
        print_process (args);

      // If diag is false, then redirect stderr to stdout.
      //
      auto_fd nfd (diag ? nullfd : open_null ());

      if (i != 0)
      {
        dpr = process (dpp, &args[what = 0], 0,   -1, (diag ? 2 : nfd.get ()));
        tpr = process (tpp, &args[what = i], dpr, -1, (diag ? 2 : nfd.get ()));
      }
      else
      {
        dpr = process (process_exit (0)); // Successfully exited.
        tpr = process (tpp, &args[what = 0], 0, -1, (diag ? 2 : nfd.get ()));
      }

      return make_pair (move (dpr), move (tpr));
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[what] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }

  pair<process, process>
  start_extract (const common_options& co,
                 const path& a,
                 const path& f,
                 bool diag)
  {
    assert (!f.empty () && f.relative ());

    cstrings args;
    args.reserve (2);

    // -O/--to-stdout -- extract to stdout.
    //
    args.push_back ("-O");

    // On Windows neither MSYS GNU tar nor BSD tar will find the archived file
    // if its path is provided in the Windows notation.
    //
#ifdef _WIN32
    string fs (f.posix_string ());
    args.push_back (fs.c_str ());
#else
    args.push_back (f.string ().c_str ());
#endif

    return start (co, 'x', a, args, diag);
  }

  string
  extract (const common_options& o, const path& a, const path& f, bool diag)
  try
  {
    pair<process, process> pr (start_extract (o, a, f, diag));

    try
    {
      // Do not throw when eofbit is set (end of stream is reached), and
      // when failbit is set (getline() failed to extract any character).
      //
      ifdstream is (move (pr.second.in_ofd), ifdstream::badbit);

      string s (is.read_text ());
      is.close ();

      if (pr.second.wait () && pr.first.wait ())
        return s;

      // Fall through.
    }
    catch (const io_error&)
    {
      // Child exit status doesn't matter. Just wait for the process
      // completion and fall through.
      //
      pr.second.wait (); pr.first.wait (); // Check throw.
    }

    // While it is reasonable to assuming the child process issued diagnostics
    // if exited with an error status, tar, specifically, doesn't mention the
    // archive name. So print the error message whatever the child exit status
    // is, if the diagnostics is requested.
    //
    if (diag)
      error << "unable to extract " << f << " from " << a;

    throw failed ();
  }
  catch (const process_error& e)
  {
    // Note: this is not a "file can't be extracted" case, so no diag check.
    //
    fail << "unable to extract " << f << " from " << a << ": " << e << endf;
  }

  paths
  archive_contents (const common_options& o, const path& a, bool diag)
  try
  {
    pair<process, process> pr (start (o, 't', a, cstrings (), diag));

    try
    {
      paths r;

      // Do not throw when eofbit is set (end of stream reached), and
      // when failbit is set (getline() failed to extract any character).
      //
      ifdstream is (move (pr.second.in_ofd), ifdstream::badbit);

      for (string l; !eof (getline (is, l)); )
        r.emplace_back (move (l));

      is.close ();

      if (pr.second.wait () && pr.first.wait ())
        return r;

      // Fall through.
    }
    catch (const invalid_path& e)
    {
      // Just fall through if the pipeline has failed.
      //
      if (pr.second.wait () && pr.first.wait ())
      {
        if (diag)
          error << "unable to obtain contents for " << a
                << ": invalid path '" << e.path << "'";

        throw failed ();
      }

      // Fall through.
    }
    catch (const io_error&)
    {
      // Child exit status doesn't matter. Just wait for the process
      // completion and fall through.
      //
      pr.second.wait (); pr.first.wait (); // Check throw.
    }

    // While it is reasonable to assuming the child process issued diagnostics
    // if exited with an error status, tar, specifically, doesn't mention the
    // archive name. So print the error message whatever the child exit status
    // is, if the diagnostics is requested.
    //
    if (diag)
      error << "unable to obtain contents for " << a;

    throw failed ();
  }
  catch (const process_error& e)
  {
    // Note: this is not a tar error, so no diag check.
    //
    fail << "unable to obtain contents for " << a << ": " << e << endf;
  }
}
