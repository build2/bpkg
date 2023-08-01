// file      : bpkg/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/utility.hxx>

#include <libbutl/prompt.hxx>
#include <libbutl/fdstream.hxx>

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

  // Keep these directory names short, lowering the probability of hitting the
  // path length limit on Windows.
  //
  const dir_path certs_dir (dir_path (bpkg_dir) /= "certs");
  const dir_path repos_dir (dir_path (bpkg_dir) /= "repos");

  // Standard and alternative build file/directory naming schemes.
  //
  // build:
  //
  const dir_path std_build_dir      ("build");
  const dir_path std_config_dir     (dir_path (std_build_dir) /= "config");
  const path     std_bootstrap_file (dir_path (std_build_dir) /= "bootstrap.build");
  const path     std_root_file      (dir_path (std_build_dir) /= "root.build");
  const string   std_build_ext      ("build");

  // build2:
  //
  const dir_path alt_build_dir      ("build2");
  const dir_path alt_config_dir     (dir_path (alt_build_dir) /= "config");
  const path     alt_bootstrap_file (dir_path (alt_build_dir) /= "bootstrap.build2");
  const path     alt_root_file      (dir_path (alt_build_dir) /= "root.build2");
  const string   alt_build_ext      ("build2");

  const dir_path current_dir (".");

  const target_triplet host_triplet (BPKG_HOST_TRIPLET);

  map<dir_path, dir_path> tmp_dirs;

  bool keep_tmp;

  auto_rmfile
  tmp_file (const dir_path& cfg, const string& p)
  {
    auto i (tmp_dirs.find (cfg));
    assert (i != tmp_dirs.end ());
    return auto_rmfile (i->second / path::traits_type::temp_name (p),
                        !keep_tmp);
  }

  auto_rmdir
  tmp_dir (const dir_path& cfg, const string& p)
  {
    auto i (tmp_dirs.find (cfg));
    assert (i != tmp_dirs.end ());
    return auto_rmdir (i->second / dir_path (path::traits_type::temp_name (p)),
                       !keep_tmp);
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
      rm_r (d, true /* dir_itself */, 2);

    mk (d); // We shouldn't need mk_p().

    tmp_dirs[cfg] = move (d);
  }

  void
  clean_tmp (bool ignore_error)
  {
    for (const auto& d: tmp_dirs)
    {
      const dir_path& td (d.second);

      if (exists (td))
      {
        rm_r (td,
              true /* dir_itself */,
              3,
              ignore_error ? rm_error_mode::ignore : rm_error_mode::fail);
      }
    }

    tmp_dirs.clear ();
  }

  path&
  normalize (path& f, const char* what)
  {
    try
    {
      if (!f.complete ().normalized ())
        f.normalize ();
    }
    catch (const invalid_path& e)
    {
      fail << "invalid " << what << " path " << e.path;
    }
    catch (const system_error& e)
    {
      fail << "unable to obtain current directory: " << e;
    }

    return f;
  }

  dir_path&
  normalize (dir_path& d, const char* what)
  {
    try
    {
      if (!d.complete ().normalized ())
        d.normalize ();
    }
    catch (const invalid_path& e)
    {
      fail << "invalid " << what << " directory " << e.path;
    }
    catch (const system_error& e)
    {
      fail << "unable to obtain current directory: " << e;
    }

    return d;
  }

  dir_path
  current_directory ()
  {
    try
    {
      return dir_path::current_directory ();
    }
    catch (const system_error& e)
    {
      fail << "unable to obtain current directory: " << e << endf;
    }
  }

  optional<const char*> stderr_term = nullopt;
  bool stderr_term_color = false;

  bool
  yn_prompt (const string& p, char d)
  {
    try
    {
      return butl::yn_prompt (p, d);
    }
    catch (io_error&)
    {
      fail << "unable to read y/n answer from stdin" << endf;
    }
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
  rm_r (const dir_path& d, bool dir, uint16_t v, rm_error_mode m)
  {
    if (verb >= v)
      text << (dir ? "rmdir -r " : "rm -r ") << (dir ? d : d / dir_path ("*"));

    try
    {
      rmdir_r (d, dir, m == rm_error_mode::ignore);
    }
    catch (const system_error& e)
    {
      bool w (m == rm_error_mode::warn);

      (w ? warn : error) << "unable to remove " << (dir ? "" : "contents of ")
                         << "directory " << d << ": " << e;

      if (!w)
        throw failed ();
    }
  }

  bool
  mv (const dir_path& from, const dir_path& to, bool ie)
  {
    if (verb >= 3)
      text << "mv " << from << ' ' << to; // Prints trailing slashes.

    try
    {
      mvdir (from, to);
    }
    catch (const system_error& e)
    {
      error << "unable to move directory " << from << " to " << to << ": "
            << e;

      if (ie)
        return false;

      throw failed ();
    }

    return true;
  }

  bool
  mv (const path& from, const path& to, bool ie)
  {
    if (verb >= 3)
      text << "mv " << from << ' ' << to;

    try
    {
      mvfile (from, to,
              cpflags::overwrite_content | cpflags::overwrite_permissions);
    }
    catch (const system_error& e)
    {
      error << "unable to move file " << from << " to " << to << ": " << e;

      if (ie)
        return false;

      throw failed ();
    }

    return true;
  }

  dir_path
  change_wd (const dir_path& d)
  {
    try
    {
      dir_path r (dir_path::current_directory ());

      if (verb >= 3)
        text << "cd " << d; // Prints trailing slash.

      dir_path::current_directory (d);
      return r;
    }
    catch (const system_error& e)
    {
      fail << "unable to change current directory to " << d << ": " << e
           << endf;
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
  open_null ()
  {
    try
    {
      return fdopen_null ();
    }
    catch (const io_error& e)
    {
      fail << "unable to open null device: " << e << endf;
    }
  }

  dir_path exec_dir;

  const char*
  name_b (const common_options& co)
  {
    return co.build_specified ()
      ? co.build ().string ().c_str ()
      : BPKG_EXE_PREFIX "b" BPKG_EXE_SUFFIX;
  }

  process_path
  search_b (const common_options& co)
  {
    const char* b (name_b (co));

    try
    {
      // Use our executable directory as a fallback search since normally the
      // entire toolchain is installed into one directory. This way, for
      // example, if we installed into /opt/build2 and run bpkg with absolute
      // path (and without PATH), then bpkg will be able to find "its" b.
      //
      return process::path_search (b, true /* init */, exec_dir);
    }
    catch (const process_error& e)
    {
      fail << "unable to execute " << b << ": " << e << endf;
    }
  }

  void
  dump_stderr (auto_fd&& fd)
  {
    ifdstream is (move (fd), fdstream_mode::skip, ifdstream::badbit);

    // We could probably write something like this, instead:
    //
    // *diag_stream << is.rdbuf () << flush;
    //
    // However, it would never throw and we could potentially miss the reading
    // failure, unless we decide to additionally mess with the diagnostics
    // stream exception mask.
    //
    for (string l; !eof (getline (is, l)); )
      *diag_stream << l << endl;

    is.close ();
  }
}
