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
    pager (const string& name)
    {
      // First try less.
      //
      try
      {
        string prompt ("-Ps" + name + " (press q to quit, h for help)");

        const char* args[] = {
          "less",
          "-R",                 // ANSI color
          prompt.c_str (),
          nullptr
        };

        p_ = process (args, -1); // Redirect child's stdin to a pipe.

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
        }
        else
          os_.open (p_.out_fd);
      }
      catch (const process_error& e)
      {
        if (e.child ())
          exit (1);
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
  help (const help_options&, const string& t, usage_function* usage)
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

    pager p ("bpkg " + (t.empty () ? "help" : t));
    usage (p.stream (), cli::usage_para::none);

    // If the pager failed, assume it has issued some diagnostics.
    //
    return p.wait () ? 0 : 1;
  }
}
