// file      : bpkg/bpkg.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>
#include <exception>

#include <bpkg/types>
#include <bpkg/diagnostics>

#include <bpkg/bpkg-options>

#include <bpkg/help>
#include <bpkg/help-options>

//#include <bpkg/rep-create>
#include <bpkg/rep-create-options>

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

  // Trace verbosity.
  //
  verb = o.verbose () > 0 ? o.verbose () : (o.v () ? 1 : 0);

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
        return help (ho, cmd_argv[1], nullptr);
    }
    else
      return help (ho, "", nullptr);
  }

  // Handle commands.
  //

  // help
  //
  if (cmd.help ())
  {
    assert (h);
    return help (ho, "help", help_options::print_usage);
  }

  // rep-create
  //
  if (cmd.rep_create ())
  {
    if (h)
      return help (ho, "rep-create", rep_create_options::print_usage);

    auto o (parse<rep_create_options> (co, args));

    if (verb)
      text << "rep-create";

    return 0;
  }

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
