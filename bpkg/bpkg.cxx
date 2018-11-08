// file      : bpkg/bpkg.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef _WIN32
#  include <signal.h> // signal()
#endif

#include <cstring>   // strcmp()
#include <iostream>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/diagnostics.hxx>
#include <bpkg/bpkg-options.hxx>

// Commands.
//
#include <bpkg/help.hxx>

#include <bpkg/cfg-create.hxx>

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
  int
  main (int argc, char* argv[]);
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

// Initialize the command option class O with the common options and then
// parse the rest of the command line placing non-option arguments to args.
// Once this is done, use the "final" values of the common options to do
// global initializations (verbosity level, etc).
//
template <typename O>
static O
init (const common_options& co,
      cli::group_scanner& scan,
      strings& args,
      bool keep_sep,
      bool tmp)
{
  O o;
  static_cast<common_options&> (o) = co;

  // We want to be able to specify options and arguments in any order (it is
  // really handy to just add -v at the end of the command line).
  //
  for (bool opt (true); scan.more (); )
  {
    if (opt)
    {
      // If we see first "--", then we are done parsing options.
      //
      if (strcmp (scan.peek (), "--") == 0)
      {
        if (!keep_sep)
          scan.next ();

        opt = false;
        continue;
      }

      // Parse the next chunk of options until we reach an argument (or eos).
      //
      if (o.parse (scan))
        continue;

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

  // Global initializations.
  //

  // Diagnostics verbosity.
  //
  verb = o.verbose_specified ()
    ? o.verbose ()
    : o.V () ? 3 : o.v () ? 2 : o.quiet () ? 0 : 1;

  // Temporary directory.
  //
  if (tmp)
    init_tmp (dir_path (cfg_dir (&o)));

  return o;
}

int bpkg::
main (int argc, char* argv[])
try
{
  using namespace cli;

  stderr_term = fdterm (stderr_fd ());
  exec_dir = path (argv[0]).directory ();

  // This is a little hack to make our baseutils for Windows work when called
  // with absolute path. In a nutshell, MSYS2's exec*p() doesn't search in the
  // parent's executable directory, only in PATH. And since we are running
  // without a shell (that would read /etc/profile which sets PATH to some
  // sensible values), we are only getting Win32 PATH values. And MSYS2 /bin
  // is not one of them. So what we are going to do is add /bin at the end of
  // PATH (which will be passed as is by the MSYS2 machinery). This will make
  // MSYS2 search in /bin (where our baseutils live). And for everyone else
  // this should be harmless since it is not a valid Win32 path.
  //
#ifdef _WIN32
  {
    string mp;
    if (optional<string> p = getenv ("PATH"))
    {
      mp = move (*p);
      mp += ';';
    }
    mp += "/bin";

    setenv ("PATH", mp);
  }
#endif

  // On POSIX ignore SIGPIPE which is signaled to a pipe-writing process if
  // the pipe reading end is closed. Note that by default this signal
  // terminates a process. Also note that there is no way to disable this
  // behavior on a file descriptor basis or for the write() function call.
  //
#ifndef _WIN32
  if (signal (SIGPIPE, SIG_IGN) == SIG_ERR)
    fail << "unable to ignore broken pipe (SIGPIPE) signal: "
         << system_error (errno, generic_category ()); // Sanitize.
#endif

  argv_file_scanner argv_scan (argc, argv, "--options-file");
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
         << "Copyright (c) 2014-2018 Code Synthesis Ltd" << endl
         << "This is free software released under the MIT license." << endl;
    return 0;
  }

  strings argsv; // To be filled by parse() above.
  vector_scanner vect_args (argsv);
  group_scanner args (vect_args);

  const common_options& co (o);

  if (o.help ())
    return help (init<help_options> (co,
                                     scan,
                                     argsv,
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
                             argsv,
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
    //                                               false /* keep_sep */,
    //                                               true  /* tmp */),
    //                     args);
    //
    //  break;
    // }
    //
#define COMMAND_IMPL(NP, SP, CMD, SEP, TMP)                               \
    if (cmd.NP##CMD ())                                                   \
    {                                                                     \
      if (h)                                                              \
        r = help (ho, SP#CMD, print_bpkg_##NP##CMD##_usage);              \
      else                                                                \
        r = NP##CMD (init<NP##CMD##_options> (co, scan, argsv, SEP, TMP), \
                     args);                                               \
                                                                          \
      break;                                                              \
    }

    // cfg-* commands
    //
#define CFG_COMMAND(CMD, TMP) COMMAND_IMPL(cfg_, "cfg-", CMD, false, TMP)

    CFG_COMMAND (create, false); // Temp dir initialized manually.

    // pkg-* commands
    //
#define PKG_COMMAND(CMD, SEP) COMMAND_IMPL(pkg_, "pkg-", CMD, SEP, true)

    PKG_COMMAND (build,     true);  // Keeps the '--' separator in args.
    PKG_COMMAND (checkout,  false);
    PKG_COMMAND (clean,     false);
    PKG_COMMAND (configure, false);
    PKG_COMMAND (disfigure, false);
    PKG_COMMAND (drop,      false);
    PKG_COMMAND (fetch,     false);
    PKG_COMMAND (install,   false);
    PKG_COMMAND (purge,     false);
    PKG_COMMAND (status,    false);
    PKG_COMMAND (test,      false);
    PKG_COMMAND (uninstall, false);
    PKG_COMMAND (unpack,    false);
    PKG_COMMAND (update,    false);
    PKG_COMMAND (verify,    false);

    // rep-* commands
    //
#define REP_COMMAND(CMD) COMMAND_IMPL(rep_, "rep-", CMD, false, true)

    REP_COMMAND (add);
    REP_COMMAND (create);
    REP_COMMAND (fetch);
    REP_COMMAND (info);
    REP_COMMAND (list);
    REP_COMMAND (remove);

    assert (false);
    fail << "unhandled command";
  }
  catch (const failed&)
  {
    r = 1;
    break;
  }

  clean_tmp (true /* ignore_error */);

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
catch (const failed&)
{
  return 1; // Diagnostics has already been issued.
}
catch (const cli::exception& e)
{
  error << e;
  return 1;
}
/*
catch (const std::exception& e)
{
  error << e;
  return 1;
}
*/

int
main (int argc, char* argv[])
{
  return bpkg::main (argc, argv);
}
