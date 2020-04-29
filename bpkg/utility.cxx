// file      : bpkg/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/utility.hxx>

#include <libbutl/prompt.mxx>
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

  // Keep these directory names short, lowering the probability of hitting the
  // path length limit on Windows.
  //
  const dir_path certs_dir (dir_path (bpkg_dir) /= "certs");
  const dir_path repos_dir (dir_path (bpkg_dir) /= "repos");

  const dir_path current_dir (".");

  dir_path temp_dir;

  auto_rmfile
  tmp_file (const string& p)
  {
    assert (!temp_dir.empty ());
    return auto_rmfile (temp_dir / path::traits_type::temp_name (p));
  }

  auto_rmdir
  tmp_dir (const string& p)
  {
    assert (!temp_dir.empty ());
    return auto_rmdir (temp_dir / dir_path (path::traits_type::temp_name (p)));
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

    temp_dir = move (d);
  }

  void
  clean_tmp (bool ignore_error)
  {
    if (!temp_dir.empty () && exists (temp_dir))
    {
      rm_r (temp_dir,
            true /* dir_itself */,
            3,
            ignore_error ? rm_error_mode::ignore : rm_error_mode::fail);

      temp_dir.clear ();
    }
  }

  path&
  normalize (path& f, const char* what)
  {
    try
    {
      f.complete ().normalize ();
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
      d.complete ().normalize ();
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

  bool stderr_term;

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
      : "b" BPKG_EXE_SUFFIX;
  }
}
