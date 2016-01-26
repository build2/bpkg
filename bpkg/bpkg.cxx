// file      : bpkg/bpkg.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <iostream>
#include <exception>

#include <bpkg/types>
#include <bpkg/utility>
#include <bpkg/diagnostics>

#include <bpkg/bpkg-options>
#include <bpkg/bpkg-version>

// Commands.
//
#include <bpkg/help>

#include <bpkg/pkg-build>
#include <bpkg/pkg-clean>
#include <bpkg/pkg-configure>
#include <bpkg/pkg-disfigure>
#include <bpkg/pkg-drop>
#include <bpkg/pkg-fetch>
#include <bpkg/pkg-install>
#include <bpkg/pkg-purge>
#include <bpkg/pkg-status>
#include <bpkg/pkg-test>
#include <bpkg/pkg-uninstall>
#include <bpkg/pkg-unpack>
#include <bpkg/pkg-update>
#include <bpkg/pkg-verify>

#include <bpkg/cfg-add>
#include <bpkg/cfg-create>
#include <bpkg/cfg-fetch>

#include <bpkg/rep-info>
#include <bpkg/rep-create>

using namespace std;
using namespace bpkg;

// Initialize the command option class O with the common options
// and then parse the rest of the arguments. Once this is done,
// use the "final" values of the common options to do global
// initializations (verbosity level, etc).
//
template <typename O>
static O
parse (const common_options& co, cli::scanner& s)
{
  O o;
  static_cast<common_options&> (o) = co;
  o.parse (s);

  // Global initializations.
  //

  // Diagnostics verbosity.
  //
  verb = o.verbose_specified () ? o.verbose () : o.v () ? 2 : o.q () ? 0 : 1;

  return o;
}

int
main (int argc, char* argv[])
try
{
  using namespace cli;

  argv_file_scanner args (argc, argv, "--options-file");

  // First parse common options and --version/--help.
  //
  options o;
  o.parse (args, unknown_mode::stop);

  if (o.version ())
  {
    cout << "bpkg " << BPKG_VERSION_STR << endl
         << "libbpkg " << LIBBPKG_VERSION_STR << endl
         << "libbutl " << LIBBUTL_VERSION_STR << endl
         << "Copyright (c) 2014-2016 Code Synthesis Ltd" << endl
         << "This is free software released under the MIT license." << endl;
    return 0;
  }

  if (o.help ())
    return help (help_options (), "", nullptr);

  const common_options& co (o);

  // The next argument should be a command.
  //
  if (!args.more ())
    fail << "bpkg command expected" <<
      info << "run 'bpkg help' for more information";

  int cmd_argc (2);
  char* cmd_argv[] {argv[0], const_cast<char*> (args.next ())};
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
    ho = parse<help_options> (co, args);

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
  for (;;)
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
    //  if (h)
    //    r = help (ho, "pkg-verify", print_bpkg_pkg_verify_usage);
    //  else
    //    r = pkg_verify (parse<pkg_verify_options> (co, args), args);
    //
    //  return 0;
    // }
    //
#define COMMAND_IMPL(NP, SP, CMD)                                       \
    if (cmd.NP##CMD ())                                                 \
    {                                                                   \
      if (h)                                                            \
        r = help (ho, SP#CMD, print_bpkg_##NP##CMD##_usage);            \
      else                                                              \
        r = NP##CMD (parse<NP##CMD##_options> (co, args), args);        \
                                                                        \
      break;                                                            \
    }

    // pkg-* commands
    //
#define PKG_COMMAND(CMD) COMMAND_IMPL(pkg_, "pkg-", CMD)

    PKG_COMMAND (build);
    PKG_COMMAND (clean);
    PKG_COMMAND (configure);
    PKG_COMMAND (disfigure);
    PKG_COMMAND (drop);
    PKG_COMMAND (fetch);
    PKG_COMMAND (install);
    PKG_COMMAND (purge);
    PKG_COMMAND (status);
    PKG_COMMAND (test);
    PKG_COMMAND (uninstall);
    PKG_COMMAND (unpack);
    PKG_COMMAND (update);
    PKG_COMMAND (verify);

    // cfg-* commands
    //
#define CFG_COMMAND(CMD) COMMAND_IMPL(cfg_, "cfg-", CMD)

    CFG_COMMAND (add);
    CFG_COMMAND (create);
    CFG_COMMAND (fetch);

    // rep-* commands
    //
#define REP_COMMAND(CMD) COMMAND_IMPL(rep_, "rep-", CMD)

    REP_COMMAND (info);
    REP_COMMAND (create);

    assert (false);
    fail << "unhandled command";
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
catch (const failed&)
{
  return 1; // Diagnostics has already been issued.
}
catch (const cli::exception& e)
{
  *diag_stream << "error: ";
  e.print (*diag_stream);
  *diag_stream << endl;
  return 1;
}
/*
catch (const std::exception& e)
{
  error << e.what ();
  return 1;
}
*/
