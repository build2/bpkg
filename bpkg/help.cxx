// file      : bpkg/help.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/help>

#ifndef _WIN32
#  include <unistd.h>    // close()
#else
#  include <io.h>        // _close()
#endif

#include <chrono>
#include <thread>    // this_thread::sleep_for()
#include <iostream>

#include <butl/process>
#include <butl/fdstream>

#include <bpkg/types>
#include <bpkg/utility>
#include <bpkg/diagnostics>

#include <bpkg/bpkg-options>

using namespace std;
using namespace butl;

namespace bpkg
{
  struct pager
  {
    pager (const common_options& co, const string& name)
    {
      cstrings args;
      string prompt;

      bool up (co.pager_specified ()); // User's pager.

      if (up)
      {
        if (co.pager ().empty ())
          return; // No pager should be used.

        args.push_back (co.pager ().c_str ());
      }
      else
      {
        // By default try less.
        //
        prompt = "-Ps" + name + " (press q to quit, h for help)";

        args.push_back ("less");
        args.push_back ("-R");            // Handle ANSI color.
        args.push_back (prompt.c_str ());
      }

      // Add extra pager options.
      //
      for (const string& o: co.pager_option ())
        args.push_back (o.c_str ());

      args.push_back (nullptr);

      if (verb >= 2)
        print_process (args);

      // Ignore errors and go without a pager unless the pager was specified
      // by the user.
      //
      try
      {
        p_ = process (args.data (), -1); // Redirect child's stdin to a pipe.

        // Wait a bit and see if the pager has exited before reading
        // anything (e.g., because exec() couldn't find the program).
        // If you know a cleaner way to handle this, let me know (no,
        // a select()-based approach doesn't work; the pipe is buffered
        // and therefore is always ready for writing).
        //
        this_thread::sleep_for (chrono::milliseconds (50));

        bool r;
        if (p_.try_wait (r))
        {
#ifndef _WIN32
          ::close (p_.out_fd);
#else
          _close (p_.out_fd);
#endif
          if (up)
            fail << "pager " << args[0] << " exited unexpectedly";
        }
        else
          os_.open (p_.out_fd);
      }
      catch (const process_error& e)
      {
        // Ignore unless it was a user-specified pager.
        //
        if (up)
          error << "unable to execute " << args[0] << ": " << e.what ();

        if (e.child ())
          exit (1);

        if (up)
          throw failed ();
      }
    }

    std::ostream&
    stream ()
    {
      return os_.is_open () ? os_ : std::cout;
    }

    bool
    wait ()
    {
      os_.close ();
      return p_.wait ();
    }

    ~pager () {wait ();}

  private:
    process p_;
    ofdstream os_;
  };

  int
  help (const help_options& o, const string& t, usage_function* usage)
  {
    if (usage == nullptr) // Not a command.
    {
      if (t.empty ())             // General help.
        usage = &print_bpkg_usage;
      else if (t == "common-options")  // Help topics.
        usage = &print_bpkg_common_options_long_usage;
      else
        fail << "unknown bpkg command/help topic '" << t << "'" <<
          info << "run 'bpkg help' for more information";
    }

    pager p (o, "bpkg " + (t.empty () ? "help" : t));
    usage (p.stream (), cli::usage_para::none);

    // If the pager failed, assume it has issued some diagnostics.
    //
    return p.wait () ? 0 : 1;
  }
}
