// file      : bpkg/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_UTILITY_HXX
#define BPKG_UTILITY_HXX

#include <map>
#include <memory>    // make_shared()
#include <string>    // to_string()
#include <cstring>   // strcmp(), strchr()
#include <utility>   // move(), forward(), declval(), make_pair()
#include <cassert>   // assert()
#include <iterator>  // make_move_iterator(), back_inserter()
#include <algorithm> // *

#include <libbutl/ft/lang.hxx>

#include <libbutl/utility.hxx>         // icasecmp(), reverse_iterate(), etc
#include <libbutl/process.hxx>
#include <libbutl/filesystem.hxx>
#include <libbutl/default-options.hxx>

#include <bpkg/types.hxx>
#include <bpkg/version.hxx>
#include <bpkg/common-options.hxx>

namespace bpkg
{
  using std::move;
  using std::forward;
  using std::declval;

  using std::make_pair;
  using std::make_shared;
  using std::make_move_iterator;
  using std::back_inserter;
  using std::to_string;

  using std::strcmp;
  using std::strchr;

  // <libbutl/utility.hxx>
  //
  using butl::icasecmp;
  using butl::reverse_iterate;

  using butl::alpha;
  using butl::alnum;
  using butl::digit;
  using butl::xdigit;

  using butl::trim;
  using butl::trim_left;
  using butl::trim_right;
  using butl::next_word;

  using butl::make_guard;
  using butl::make_exception_guard;

  using butl::getenv;
  using butl::setenv;
  using butl::unsetenv;

  using butl::eof;

  // <libbutl/process.hxx>
  //
  using butl::process_start_callback;
  using butl::process_print_callback;

  // <libbutl/filesystem.hxx>
  //
  using butl::auto_rmfile;
  using butl::auto_rmdir;

  // <libbutl/default-options.hxx>
  //
  using butl::load_default_options;
  using butl::merge_default_options;

  // Empty string and path.
  //
  extern const string empty_string;
  extern const path empty_path;
  extern const dir_path empty_dir_path;

  // Widely-used paths.
  //
  extern const dir_path bpkg_dir;           // .bpkg/
  extern const dir_path certs_dir;          // .bpkg/certs/
  extern const dir_path repos_dir;          // .bpkg/repos/

  extern const dir_path std_build_dir;      // build/
  extern const dir_path std_config_dir;     // build/config/
  extern const path     std_bootstrap_file; // build/bootstrap.build
  extern const path     std_root_file;      // build/root.build
  extern const string   std_build_ext;      // build

  extern const dir_path alt_build_dir;      // build2/
  extern const dir_path alt_config_dir;     // build2/config/
  extern const path     alt_bootstrap_file; // build2/bootstrap.build2
  extern const path     alt_root_file;      // build2/root.build2
  extern const string   alt_build_ext;      // build2

  extern const dir_path current_dir;        // ./

  // Host target triplet for which we were built.
  //
  extern const target_triplet host_triplet;

  // Temporary directory facility.
  //
  // An entry normally maps <cfg-dir> to <cfg-dir>/.bpkg/tmp/ but can also map
  // an empty directory to some system-wide directory (e.g., /tmp/bpkg-XXX/)
  // if there is no bpkg configuration. The temporary directory for the
  // current configuration is automatically created and cleaned up for most
  // commands in main(), so you don't need to call init_tmp() explicitly
  // except for certain special commands (like cfg-create).
  //
  extern std::map<dir_path, dir_path> tmp_dirs;

  extern bool keep_tmp; // --keep-tmp

  auto_rmfile
  tmp_file (const dir_path& cfg, const string& prefix);

  auto_rmdir
  tmp_dir (const dir_path& cfg, const string& prefix);

  void
  init_tmp (const dir_path& cfg);

  void
  clean_tmp (bool ignore_errors);

  // Path.
  //
  // Normalize a path. Also make the relative path absolute using the current
  // directory.
  //
  path&
  normalize (path&, const char* what);

  inline path
  normalize (const path& f, const char* what)
  {
    path r (f);
    return move (normalize (r, what));
  }

  dir_path&
  normalize (dir_path&, const char* what);

  inline dir_path
  normalize (const dir_path& d, const char* what)
  {
    dir_path r (d);
    return move (normalize (r, what));
  }

  dir_path
  current_directory ();

  // Diagnostics.
  //
  // If stderr is not a terminal, then the value is absent (so can be used as
  // bool). Otherwise, it is the value of the TERM environment variable (which
  // can be NULL).
  //
  extern optional<const char*> stderr_term;

  // True if the color can be used on the stderr terminal.
  //
  extern bool stderr_term_color;

  // Y/N prompt. See butl::yn_prompt() for details (this is a thin wrapper).
  //
  // Issue diagnostics and throw failed if no answer could be extracted from
  // stdin (e.g., because it was closed).
  //
  bool
  yn_prompt (const string& prompt, char def = '\0');

  // Filesystem.
  //
  bool
  exists (const path&, bool ignore_error = false);

  bool
  exists (const dir_path&, bool ignore_error = false);

  bool
  empty (const dir_path&);

  void
  mk (const dir_path&);

  void
  mk_p (const dir_path&);

  void
  rm (const path&, uint16_t verbosity = 3);

  enum class rm_error_mode {ignore, warn, fail};

  void
  rm_r (const dir_path&,
        bool dir_itself = true,
        uint16_t verbosity = 3,
        rm_error_mode = rm_error_mode::fail);

  // Note that if ignore_error is true, the diagnostics is still issued.
  //
  bool
  mv (const dir_path& from, const dir_path& to, bool ignore_errors = false);

  bool
  mv (const path& from, const path& to, bool ignore_errors = false);

  // Set (with diagnostics at verbosity level 3 or higher) the new and return
  // the previous working directory.
  //
  dir_path
  change_wd (const dir_path&);

  // File descriptor streams.
  //
  fdpipe
  open_pipe ();

  auto_fd
  open_null ();

  // Directory extracted from argv[0] (i.e., this process' recall directory)
  // or empty if there is none. Can be used as a search fallback.
  //
  extern dir_path exec_dir;

  // Run build2, mapping verbosity levels.
  //

  // Verbosity level 1 mapping.
  //
  enum class verb_b
  {
    quiet,    // Run quiet.
    progress, // Run quiet but (potentially) with progress.
    normal    // Run normally (at verbosity 1).
  };

  template <typename V>
  void
  map_verb_b (const common_options&, verb_b, V& args, string& verb_arg);

  const char*
  name_b (const common_options&);

  process_path
  search_b (const common_options&);

  template <typename... A>
  void
  print_b (const common_options&, verb_b, A&&... args);

  template <typename O, typename E, typename... A>
  process
  start_b (const common_options&, O&& out, E&& err, verb_b, A&&... args);

  template <typename... A>
  void
  run_b (const common_options&, verb_b, A&&... args);

  // Read out the data from the specified file descriptor and dump it to
  // stderr. Throw io_error on the underlying OS errors.
  //
  void
  dump_stderr (auto_fd&&);
}

#include <bpkg/utility.txx>

#endif // BPKG_UTILITY_HXX
