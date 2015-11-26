// file      : bpkg/help.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/help>

#ifndef _WIN32
#  include <unistd.h>    // close(), STDOUT_FILENO
#  include <sys/ioctl.h> // ioctl()
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
  struct pager: protected std::streambuf
  {
    pager (const common_options& co, const string& name)
    {
      bool up (co.pager_specified ()); // User's pager.

      // If we are using the default pager, try to get the terminal width
      // so that we can center the output.
      //
      if (!up)
      {
        size_t col (0);

#ifndef _WIN32
#  ifdef TIOCGWINSZ
        struct winsize w;
        if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
          col = static_cast<size_t> (w.ws_col);
#  endif
#else
#endif
        if (col > 80)
          indent_.assign ((col - 80) / 2, ' ');
      }

      cstrings args;
      string prompt;

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

      // Setup the indentation machinery.
      //
      if (!indent_.empty ())
        buf_ = stream ().rdbuf (this);
    }

    std::ostream&
    stream ()
    {
      return os_.is_open () ? os_ : std::cout;
    }

    bool
    wait ()
    {
      // Teardown the indentation machinery.
      //
      if (buf_ != nullptr)
      {
        stream ().rdbuf (buf_);
        buf_ = nullptr;
      }

      os_.close ();
      return p_.wait ();
    }

    ~pager () {wait ();}

    // streambuf output interface.
    //
  protected:
    using int_type = std::streambuf::int_type;
    using traits_type = std::streambuf::traits_type;

    virtual int_type
    overflow (int_type c)
    {
      if (prev_ == '\n' && c != '\n') // Don't indent blanks.
      {
        auto n (static_cast<streamsize> (indent_.size ()));

        if (buf_->sputn (indent_.c_str (), n) != n)
          return traits_type::eof ();
      }

      prev_ = c;
      return buf_->sputc (c);
    }

    virtual int
    sync ()
    {
      return buf_->pubsync ();
    }

  private:
    process p_;
    ofdstream os_;

    string indent_;
    int_type prev_ = '\n'; // Previous character.
    std::streambuf* buf_ = nullptr;
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
