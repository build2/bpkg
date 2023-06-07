// file      : bpkg/bpkg.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/bpkg.hxx>

#include <limits>
#include <cstdlib>     // getenv()
#include <cstring>     // strcmp()
#include <iostream>
#include <exception>   // set_terminate(), terminate_handler
#include <type_traits> // enable_if, is_base_of

#include <libbutl/backtrace.hxx> // backtrace()

// Embedded build system driver.
//
#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>
#include <libbuild2/module.hxx>

#include <libbuild2/b-options.hxx>
#include <libbuild2/b-cmdline.hxx>

#include <libbuild2/dist/init.hxx>
#include <libbuild2/test/init.hxx>
#include <libbuild2/config/init.hxx>
#include <libbuild2/install/init.hxx>

#include <libbuild2/in/init.hxx>
#include <libbuild2/bin/init.hxx>
#include <libbuild2/c/init.hxx>
#include <libbuild2/cc/init.hxx>
#include <libbuild2/cxx/init.hxx>
#include <libbuild2/version/init.hxx>

#include <libbuild2/bash/init.hxx>
#include <libbuild2/cli/init.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/diagnostics.hxx>
#include <bpkg/bpkg-options.hxx>

// Commands.
//
#include <bpkg/help.hxx>

#include <bpkg/cfg-create.hxx>
#include <bpkg/cfg-info.hxx>
#include <bpkg/cfg-link.hxx>
#include <bpkg/cfg-unlink.hxx>

#include <bpkg/pkg-bindist.hxx>
#include <bpkg/pkg-build.hxx>
#include <bpkg/pkg-checkout.hxx>
#include <bpkg/pkg-clean.hxx>
#include <bpkg/pkg-configure.hxx>
#include <bpkg/pkg-disfigure.hxx>
#include <bpkg/pkg-drop.hxx>
#include <bpkg/pkg-fetch.hxx>
#include <bpkg/pkg-install.hxx>
#include <bpkg/pkg-purge.hxx>
#include <bpkg/pkg-status.hxx>
#include <bpkg/pkg-test.hxx>
#include <bpkg/pkg-uninstall.hxx>
#include <bpkg/pkg-unpack.hxx>
#include <bpkg/pkg-update.hxx>
#include <bpkg/pkg-verify.hxx>

#include <bpkg/rep-add.hxx>
#include <bpkg/rep-create.hxx>
#include <bpkg/rep-fetch.hxx>
#include <bpkg/rep-info.hxx>
#include <bpkg/rep-list.hxx>
#include <bpkg/rep-remove.hxx>

using namespace std;
using namespace butl;
using namespace bpkg;

namespace bpkg
{
  // Print backtrace if terminating due to an unhandled exception. Note that
  // custom_terminate is non-static and not a lambda to reduce the noise.
  //
  static terminate_handler default_terminate;

  void
  custom_terminate ()
  {
    *diag_stream << backtrace ();

    if (default_terminate != nullptr)
      default_terminate ();
  }

  static void
  build2_terminate (bool trace)
  {
    if (!trace)
      set_terminate (default_terminate);

    std::terminate ();
  }

  strings                build2_cmd_vars;
  build2::scheduler      build2_sched;
  build2::global_mutexes build2_mutexes;
  build2::file_cache     build2_fcache;

  static const char*     build2_argv0;

  void
  build2_init (const common_options& co)
  {
    try
    {
      using namespace build2;
      using build2::fail;
      using build2::endf;

      build2::tracer trace ("build2_init");

      // Parse --build-option values as the build2 driver command line.
      //
      // With things like verbosity, progress, etc., we use values from
      // --build-option if specified, falling back to equivalent bpkg values
      // otherwise.
      //
      b_options bo;
      b_cmdline bc;
      {
        small_vector<char*, 1> argv {const_cast<char*> (build2_argv0)};

        if (size_t n = co.build_option ().size ())
        {
          argv.reserve (n + 1);

          for (const string& a: co.build_option ())
            argv.push_back (const_cast<char*> (a.c_str ()));
        }

        // Note that this function also parses the default options files and
        // gets/sets the relevant environment variables.
        //
        // For now we use the same default verbosity as us (equivalent to
        // start_b() with verb_b::normal).
        //
        bc = parse_b_cmdline (trace,
                              static_cast<int> (argv.size ()), argv.data (),
                              bo,
                              bpkg::verb,
                              co.jobs_specified () ? co.jobs () : 0);

        if (!bc.buildspec.empty ())
          fail << "argument specified with --build-option";

        if (bo.help () || bo.version ())
          fail << "--help or --version specified with --build-option";

        // Make sure someone didn't specify a non-global override with
        // --build-option, which messes our global/package-specific config
        // variable split.
        //
        for (const string& v: bc.cmd_vars)
        {
          if (v[0] != '!')
            fail << "non-global configuration variable '" << v
                 <<  "' specified with --build-option";
        }
      }

      build2_cmd_vars = move (bc.cmd_vars);

      init_diag (bc.verbosity,
                 bo.silent (),
                 (bc.progress         ? bc.progress :
                  co.progress ()      ? optional<bool> (true)  :
                  co.no_progress ()   ? optional<bool> (false) : nullopt),
                 (bc.diag_color       ? bc.diag_color :
                  co.diag_color ()    ? optional<bool> (true)  :
                  co.no_diag_color () ? optional<bool> (false) : nullopt),
                 bo.no_line (),
                 bo.no_column (),
                 bpkg::stderr_term.has_value ());

      // Also note that we now use this in pkg_configure(), but serial-stop
      // is good for it as well.
      //
      init (&build2_terminate,
            build2_argv0,
            false /* serial_stop */,
            bc.mtime_check,
            bc.config_sub,
            bc.config_guess);

      load_builtin_module (&build2::config::build2_config_load);
      load_builtin_module (&build2::dist::build2_dist_load);
      load_builtin_module (&build2::test::build2_test_load);
      load_builtin_module (&build2::install::build2_install_load);

      load_builtin_module (&build2::bin::build2_bin_load);
      load_builtin_module (&build2::cc::build2_cc_load);
      load_builtin_module (&build2::c::build2_c_load);
      load_builtin_module (&build2::cxx::build2_cxx_load);
      load_builtin_module (&build2::version::build2_version_load);
      load_builtin_module (&build2::in::build2_in_load);

      load_builtin_module (&build2::bash::build2_bash_load);
      load_builtin_module (&build2::cli::build2_cli_load);

      // Note that while all we need is serial execution (all we do is load),
      // in the process we may need to update some build system modules (while
      // we only support built-in and standard pre-installed modules here, we
      // may need to build the latter during development). At the same time,
      // this is an unlikely case and starting a parallel scheduler is not
      // cheap. So what we will do is start a parallel scheduler pre-tuned to
      // serial execution, which is relatively cheap. The module building
      // logic will then re-tune it to parallel if and when necessary.
      //
      // Note that we now also use this in pkg_configure() where we re-tune
      // the scheduler (it may already have been initialized as part of the
      // package skeleton work).
      //
      build2_sched.startup (1 /* max_active */,
                            1 /* init_active */,
                            bc.max_jobs,
                            bc.jobs * bo.queue_depth (),
                            bc.max_stack,
                            bc.jobs);

      build2_mutexes.init (build2_sched.shard_size ());
      build2_fcache.init (bc.fcache_compress);
    }
    catch (const build2::failed&)
    {
      throw bpkg::failed (); // Assume the diagnostics has already been issued.
    }
  }

  // Deduce the default options files and the directory to start searching
  // from based on the command line options and arguments.
  //
  // default_options_files
  // options_files (const char* cmd, const xxx_options&, const strings& args);

  // Return the default options files and the configuration directory as a
  // search start directory for commands that operate on a configuration (and
  // thus have their options derived from configuration_options).
  //
  // Note that we don't support package-level default options files.
  //
  static inline default_options_files
  options_files (const char* cmd,
                 const configuration_options& o,
                 const strings&)
  {
    // bpkg.options
    // bpkg-<cmd>.options

    return default_options_files {
      {path ("bpkg.options"), path (string ("bpkg-") + cmd + ".options")},
      normalize (o.directory (), "configuration")};
  }

  // Return the default options files without search start directory for
  // commands that don't operate on a configuration (and thus their options
  // are not derived from configuration_options).
  //
  template <typename O>
  static inline typename enable_if<!is_base_of<configuration_options,
                                               O>::value,
                                   default_options_files>::type
  options_files (const char* cmd,
                 const O&,
                 const strings&)
  {
    // bpkg.options
    // bpkg-<cmd>.options

    return default_options_files {
      {path ("bpkg.options"), path (string ("bpkg-") + cmd + ".options")},
      nullopt /* start */};
  }

  // Merge the default options and the command line options. Fail if options
  // used to deduce the default options files or the start directory appear in
  // an options file (or for other good reasons).
  //
  // xxx_options
  // merge_options (const default_options<xxx_options>&, const xxx_options&);

  // Merge the default options and the command line options for commands
  // that operate on configuration. Fail if --directory|-d appears in the
  // options file to avoid the chicken and egg problem.
  //
  template <typename O>
  static inline typename enable_if<is_base_of<configuration_options,
                                              O>::value,
                                   O>::type
  merge_options (const default_options<O>& defs, const O& cmd)
  {
    return merge_default_options (
      defs,
      cmd,
      [] (const default_options_entry<O>& e, const O&)
      {
        if (e.options.directory_specified ())
          fail (e.file) << "--directory|-d in default options file";
      });
  }

  // Merge the default options and the command line options for commands that
  // allow any option in the options files (and thus their options are not
  // derived from configuration_options).
  //
  template <typename O>
  static inline typename enable_if<!is_base_of<configuration_options,
                                               O>::value,
                                   O>::type
  merge_options (const default_options<O>& defs, const O& cmd)
  {
    return merge_default_options (defs, cmd);
  }

  int
  main (int argc, char* argv[]);
}

// Note that pkg-build command supports multiple configurations and
// initializes multiple temporary directories itself. This function is,
// however, required since pkg_build_options::directory() returns a vector and
// the below template function cannot be used.
//
static inline const dir_path&
cfg_dir (const pkg_build_options*)
{
  return empty_dir_path;
}

// Get -d|--directory value if the option class O has it and empty path
// otherwise. Note that for some commands (like rep-info) that allow
// specifying empty path, the returned value is a string, not a dir_path.
//
template <typename O>
static inline auto
cfg_dir (const O* o) -> decltype(o->directory ()) {return o->directory ();}

static inline auto
cfg_dir (...) -> const dir_path& {return empty_dir_path;}

// Command line arguments starting position.
//
// We want the positions of the command line arguments to be after the default
// options files (parsed in init()). Normally that would be achieved by
// passing the last position of the previous scanner to the next. The problem
// is that we parse the command line arguments first (for good reasons). Also
// the default options files parsing machinery needs the maximum number of
// arguments to be specified and assigns the positions below this value (see
// load_default_options() for details). So we are going to "reserve" the first
// half of the size_t value range for the default options positions and the
// second half for the command line arguments positions.
//
static const size_t args_pos (numeric_limits<size_t>::max () / 2);

// Initialize the command option class O with the common options and then
// parse the rest of the command line placing non-option arguments to args.
// Once this is done, use the "final" values of the common options to do
// global initializations (verbosity level, etc).
//
template <typename O>
static O
init (const common_options& co,
      cli::group_scanner& scan,
      strings& args, cli::vector_scanner& args_scan,
      const char* cmd,
      bool keep_sep,
      bool tmp)
{
  using bpkg::optional;
  using bpkg::getenv;

  tracer trace ("init");

  O o;
  static_cast<common_options&> (o) = co;

  // We want to be able to specify options and arguments in any order (it is
  // really handy to just add -v at the end of the command line).
  //
  for (bool opt (true); scan.more (); )
  {
    if (opt)
    {
      // Parse the next chunk of options until we reach an argument (or eos).
      //
      if (o.parse (scan) && !scan.more ())
        break;

      // If we see first "--", then we are done parsing options.
      //
      if (strcmp (scan.peek (), "--") == 0)
      {
        if (!keep_sep)
          scan.next ();

        opt = false;
        continue;
      }

      // Fall through.
    }

    // Copy over the argument including the group.
    //
    using scanner = cli::scanner;
    using group_scanner = cli::group_scanner;

    args.push_back (group_scanner::escape (scan.next ()));

    scanner& gscan (scan.group ());
    if (gscan.more ())
    {
      args.push_back ("+{");
      while (gscan.more ())
        args.push_back (group_scanner::escape (gscan.next ()));
      args.push_back ("}");
    }
  }

  // Carry over the positions of the arguments. In particular, this can be
  // used to get the max position for the options.
  //
  args_scan.reset (0, scan.position ());

  // Note that the diagnostics verbosity level can only be calculated after
  // default options are loaded and merged (see below). Thus, to trace the
  // default options files search, we refer to the verbosity level specified
  // on the command line.
  //
  auto verbosity = [&o] ()
  {
    return o.verbose_specified ()
           ? o.verbose ()
           : o.V () ? 3 : o.v () ? 2 : o.quiet () ? 0 : 1;
  };

  // Load the default options files, unless --no-default-options is specified
  // on the command line or the BPKG_DEF_OPT environment variable is set to a
  // value other than 'true' or '1'.
  //
  optional<string> env_def (getenv ("BPKG_DEF_OPT"));

  // False if --no-default-options is specified on the command line. Note that
  // we cache the flag since it can be overridden by a default options file.
  //
  bool cmd_def (!o.no_default_options ());

  // Note: don't need to use group_scaner (no arguments in options files).
  //
  if (cmd_def && (!env_def || *env_def == "true" || *env_def == "1"))
  try
  {
    optional<dir_path> extra;
    if (o.default_options_specified ())
    {
      extra = o.default_options ();

      // Note that load_default_options() expects absolute and normalized
      // directory.
      //
      try
      {
        if (extra->relative ())
          extra->complete ();

        extra->normalize ();
      }
      catch (const invalid_path& e)
      {
        fail << "invalid --default-options value " << e.path;
      }
    }

    default_options<O> dos (
      load_default_options<O, cli::argv_file_scanner, cli::unknown_mode> (
        nullopt /* sys_dir */,
        path::home_directory (),
        extra,
        options_files (cmd, o, args),
        [&trace, &verbosity] (const path& f, bool r, bool o)
        {
          if (verbosity () >= 3)
          {
            if (o)
              trace << "treating " << f << " as " << (r ? "remote" : "local");
            else
              trace << "loading " << (r ? "remote " : "local ") << f;
          }
        },
        "--options-file",
        args_pos,
        1024));

    // Verify common options.
    //
    // Also merge the --*/--no-* options, overriding a less specific flag with
    // a more specific.
    //
    //
    optional<bool> progress, diag_color;
    auto merge_no = [&progress, &diag_color] (
      const O& o,
      const default_options_entry<O>* e = nullptr)
    {
      {
        if (o.progress () && o.no_progress ())
        {
          diag_record dr;
          (e != nullptr ? dr << fail (e->file) : dr << fail)
          << "both --progress and --no-progress specified";
        }

        if (o.progress ())
          progress = true;
        else if (o.no_progress ())
          progress = false;
      }

      {
        if (o.diag_color () && o.no_diag_color ())
        {
          diag_record dr;
          (e != nullptr ? dr << fail (e->file) : dr << fail)
          << "both --diag-color and --no-diag-color specified";
        }

        if (o.diag_color ())
          diag_color = true;
        else if (o.no_diag_color ())
          diag_color = false;
      }
    };

    for (const default_options_entry<O>& e: dos)
      merge_no (e.options, &e);

    merge_no (o);

    o = merge_options (dos, o);

    if (progress)
    {
      o.progress (*progress);
      o.no_progress (!*progress);
    }

    if (diag_color)
    {
      o.diag_color (*diag_color);
      o.no_diag_color (!*diag_color);
    }
  }
  catch (const invalid_argument& e)
  {
    fail << "unable to load default options files: " << e;
  }
  catch (const pair<path, system_error>& e)
  {
    fail << "unable to load default options files: " << e.first << ": "
         << e.second;
  }
  catch (const system_error& e)
  {
    fail << "unable to obtain home directory: " << e;
  }

  // Propagate disabling of the default options files to the potential nested
  // invocations.
  //
  if (!cmd_def && (!env_def || *env_def != "0"))
    setenv ("BPKG_DEF_OPT", "0");

  // Global initializations.
  //

  // Diagnostics verbosity.
  //
  verb = verbosity ();

  // Temporary directory.
  //
  if (tmp)
    init_tmp (dir_path (cfg_dir (&o)));

  keep_tmp = o.keep_tmp ();

  return o;
}

int bpkg::
main (int argc, char* argv[])
try
{
  using namespace cli;

  default_terminate = set_terminate (custom_terminate);

  if (fdterm (stderr_fd ()))
  {
    stderr_term = std::getenv ("TERM");

    stderr_term_color =
#ifdef _WIN32
      // For now we disable color on Windows since it's unclear if/where/how
      // it is supported. Maybe one day someone will figure this out.
      //
      false
#else
      // This test was lifted from GCC (Emacs shell sets TERM=dumb).
      //
      *stderr_term != nullptr && strcmp (*stderr_term, "dumb") != 0
#endif
      ;
  }

  exec_dir = path (argv[0]).directory ();
  build2_argv0 = argv[0];

  // Note that this call sets PATH to include our baseutils /bin on Windows
  // and ignores SIGPIPE.
  //
  build2::init_process ();

  argv_file_scanner argv_scan (argc, argv, "--options-file", false, args_pos);
  group_scanner scan (argv_scan);

  // First parse common options and --version/--help.
  //
  options o;
  o.parse (scan, unknown_mode::stop);

  if (o.version ())
  {
    cout << "bpkg " << BPKG_VERSION_ID << endl
         << "libbpkg " << LIBBPKG_VERSION_ID << endl
         << "libbutl " << LIBBUTL_VERSION_ID << endl
         << "host " << host_triplet << endl
         << "Copyright (c) " << BPKG_COPYRIGHT << "." << endl
         << "This is free software released under the MIT license." << endl;
    return 0;
  }

  strings argsv; // To be filled by init() above.
  vector_scanner scanv (argsv);
  group_scanner args (scanv);

  const common_options& co (o);

  if (o.help ())
    return help (init<help_options> (co,
                                     scan,
                                     argsv, scanv,
                                     "help",
                                     false /* keep_sep */,
                                     false /* tmp */),
                 "",
                 nullptr);

  // The next argument should be a command.
  //
  if (!scan.more ())
    fail << "bpkg command expected" <<
      info << "run 'bpkg help' for more information";

  int cmd_argc (2);
  char* cmd_argv[] {argv[0], const_cast<char*> (scan.next ())};
  commands cmd;
  cmd.parse (cmd_argc, cmd_argv, true, unknown_mode::stop);

  if (cmd_argc != 1)
    fail << "unknown bpkg command/option '" << cmd_argv[1] << "'" <<
      info << "run 'bpkg help' for more information";

  // If the command is 'help', then what's coming next is another
  // command. Parse it into cmd so that we only need to check for
  // each command in one place.
  //
  bool h (cmd.help ());
  help_options ho;

  if (h)
  {
    ho = init<help_options> (co,
                             scan,
                             argsv, scanv,
                             "help",
                             false /* keep_sep */,
                             false /* tmp */);

    if (args.more ())
    {
      cmd_argc = 2;
      cmd_argv[1] = const_cast<char*> (args.next ());

      // First see if this is a command.
      //
      cmd = commands (); // Clear the help option.
      cmd.parse (cmd_argc, cmd_argv, true, unknown_mode::stop);

      // If not, then it got to be a help topic.
      //
      if (cmd_argc != 1)
        return help (ho, cmd_argv[1], nullptr);
    }
    else
      return help (ho, "", nullptr);
  }

  // Handle commands.
  //
  int r (1);
  for (;;) // Breakout loop.
  try
  {
    // help
    //
    if (cmd.help ())
    {
      assert (h);
      r = help (ho, "help", print_bpkg_help_usage);
      break;
    }

    // Commands.
    //
    // if (cmd.pkg_verify ())
    // {
    //   if (h)
    //     r = help (ho, "pkg-verify", print_bpkg_pkg_verify_usage);
    //   else
    //     r = pkg_verify (init<pkg_verify_options> (co,
    //                                               scan,
    //                                               argsv,
    //                                               scanv,
    //                                               "pkg-verify",
    //                                               false /* keep_sep */,
    //                                               true  /* tmp */),
    //                     args);
    //
    //  break;
    // }
    //
#define COMMAND_IMPL(NP, SP, CMD, SEP, TMP)                  \
    if (cmd.NP##CMD ())                                      \
    {                                                        \
      if (h)                                                 \
        r = help (ho, SP#CMD, print_bpkg_##NP##CMD##_usage); \
      else                                                   \
        r = NP##CMD (init<NP##CMD##_options> (co,            \
                                              scan,          \
                                              argsv,         \
                                              scanv,         \
                                              SP#CMD,        \
                                              SEP,           \
                                              TMP),          \
                     args);                                  \
                                                             \
      break;                                                 \
    }

    // cfg-* commands
    //
#define CFG_COMMAND(CMD, TMP) COMMAND_IMPL(cfg_, "cfg-", CMD, false, TMP)

    CFG_COMMAND (create, false); // Temp dir initialized manually.
    CFG_COMMAND (info,   true);
    CFG_COMMAND (link,   true);
    CFG_COMMAND (unlink, true);

    // pkg-* commands
    //
#define PKG_COMMAND(CMD, SEP, TMP) COMMAND_IMPL(pkg_, "pkg-", CMD, SEP, TMP)

    // These commands need the '--' separator to be kept in args.
    //
    PKG_COMMAND (bindist,   true,  true);
    PKG_COMMAND (build,     true,  false);
    PKG_COMMAND (clean,     true,  true);
    PKG_COMMAND (configure, true,  true);
    PKG_COMMAND (install,   true,  true);
    PKG_COMMAND (test,      true,  true);
    PKG_COMMAND (uninstall, true,  true);
    PKG_COMMAND (update,    true,  true);

    PKG_COMMAND (checkout,  false, true);
    PKG_COMMAND (disfigure, false, true);
    PKG_COMMAND (drop,      false, true);
    PKG_COMMAND (fetch,     false, true);
    PKG_COMMAND (purge,     false, true);
    PKG_COMMAND (status,    false, true);
    PKG_COMMAND (unpack,    false, true);
    PKG_COMMAND (verify,    false, true);

    // rep-* commands
    //
#define REP_COMMAND(CMD, TMP) COMMAND_IMPL(rep_, "rep-", CMD, false, TMP)

    REP_COMMAND (add,    true);
    REP_COMMAND (create, true);
    REP_COMMAND (fetch,  true);
    REP_COMMAND (info,   false);
    REP_COMMAND (list,   true);
    REP_COMMAND (remove, true);

    assert (false);
    fail << "unhandled command";
  }
  catch (const failed& e)
  {
    r = e.code;
    break;
  }

  // Shutdown the build2 scheduler if it was initialized.
  //
  if (build2_sched.started ())
    build2_sched.shutdown ();

  if (!keep_tmp)
  {
    clean_tmp (true /* ignore_error */);
  }
  else if (verb > 1)
  {
    for (const auto& d: tmp_dirs)
    {
      const dir_path& td (d.second);

      if (exists (td))
        info << "keeping temporary directory " << td;
    }
  }

  if (r != 0)
    return r;

  // Warn if args contain some leftover junk. We already successfully
  // performed the command so failing would probably be misleading.
  //
  if (args.more ())
  {
    diag_record dr;
    dr << warn << "ignoring unexpected argument(s)";
    while (args.more ())
      dr << " '" << args.next () << "'";
  }

  return 0;
}
catch (const failed& e)
{
  return e.code; // Diagnostics has already been issued.
}
catch (const cli::exception& e)
{
  error << e;
  return 1;
}
#if 0
catch (const std::exception& e)
{
  error << e;
  return 1;
}
#endif

int
main (int argc, char* argv[])
{
  return bpkg::main (argc, argv);
}
