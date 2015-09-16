// file      : bpkg/bpkg.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>
#include <exception>

#include <bpkg/types>
#include <bpkg/diagnostics>

#include <bpkg/bpkg-options>

// Commands.
//
#include <bpkg/help>
#include <bpkg/pkg-verify>
#include <bpkg/pkg-fetch>
#include <bpkg/pkg-unpack>
#include <bpkg/cfg-create>
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
  bpkg_options bo;
  bo.parse (args, unknown_mode::stop);

  if (bo.version ())
  {
    cout << "bpkg 0.0.0" << endl
         << "Copyright (c) 2014-2015 Code Synthesis Ltd" << endl
         << "This is free software released under the MIT license." << endl;
    return 0;
  }

  if (bo.help ())
  {
    help (help_options (), "", nullptr);
    return 0;
  }

  const common_options& co (bo);

  // The next argument should be a command.
  //
  if (!args.more ())
    fail << "bpkg command expected" <<
      info << "run 'bpkg help' for more information";

  int cmd_argc (2);
  char* cmd_argv[] {argv[0], const_cast<char*> (args.next ())};
  bpkg_commands cmd;
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
      cmd = bpkg_commands (); // Clear help option.
      cmd.parse (cmd_argc, cmd_argv, true, unknown_mode::stop);

      // If not, then it got to be a help topic.
      //
      if (cmd_argc != 1)
      {
        help (ho, cmd_argv[1], nullptr);
        return 0;
      }
    }
    else
    {
      help (ho, "", nullptr);
      return 0;
    }
  }

  // Handle commands.
  //

  // help
  //
  if (cmd.help ())
  {
    assert (h);
    help (ho, "help", help_options::print_usage);
    return 0;
  }

  // pkg-verify
  //
  if (cmd.pkg_verify ())
  {
    if (h)
      help (ho, "pkg-verify", pkg_verify_options::print_usage);
    else
      pkg_verify (parse<pkg_verify_options> (co, args), args);

    return 0;
  }

  // pkg-fetch
  //
  if (cmd.pkg_fetch ())
  {
    if (h)
      help (ho, "pkg-fetch", pkg_fetch_options::print_usage);
    else
      pkg_fetch (parse<pkg_fetch_options> (co, args), args);

    return 0;
  }

  // pkg-unpack
  //
  if (cmd.pkg_unpack ())
  {
    if (h)
      help (ho, "pkg-unpack", pkg_unpack_options::print_usage);
    else
      pkg_unpack (parse<pkg_unpack_options> (co, args), args);

    return 0;
  }

  // cfg-create
  //
  if (cmd.cfg_create ())
  {
    if (h)
      help (ho, "cfg-create", cfg_create_options::print_usage);
    else
      cfg_create (parse<cfg_create_options> (co, args), args);

    return 0;
  }

  // rep-create
  //
  if (cmd.rep_create ())
  {
    if (h)
      help (ho, "rep-create", rep_create_options::print_usage);
    else
      rep_create (parse<rep_create_options> (co, args), args);

    return 0;
  }

  // @@ Would be nice to check that args doesn't contain any junk left.

  assert (false); // Unhandled command.
  return 1;
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
