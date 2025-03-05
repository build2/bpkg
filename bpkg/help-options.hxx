// -*- C++ -*-
//
// This file was generated by CLI, a command line interface
// compiler for C++.
//

#ifndef BPKG_HELP_OPTIONS_HXX
#define BPKG_HELP_OPTIONS_HXX

// Begin prologue.
//
//
// End prologue.

#include <bpkg/common-options.hxx>

namespace bpkg
{
  class help_options: public ::bpkg::common_options
  {
    public:
    help_options ();

    // Return true if anything has been parsed.
    //
    bool
    parse (int& argc,
           char** argv,
           bool erase = false,
           ::bpkg::cli::unknown_mode option = ::bpkg::cli::unknown_mode::fail,
           ::bpkg::cli::unknown_mode argument = ::bpkg::cli::unknown_mode::stop);

    bool
    parse (int start,
           int& argc,
           char** argv,
           bool erase = false,
           ::bpkg::cli::unknown_mode option = ::bpkg::cli::unknown_mode::fail,
           ::bpkg::cli::unknown_mode argument = ::bpkg::cli::unknown_mode::stop);

    bool
    parse (int& argc,
           char** argv,
           int& end,
           bool erase = false,
           ::bpkg::cli::unknown_mode option = ::bpkg::cli::unknown_mode::fail,
           ::bpkg::cli::unknown_mode argument = ::bpkg::cli::unknown_mode::stop);

    bool
    parse (int start,
           int& argc,
           char** argv,
           int& end,
           bool erase = false,
           ::bpkg::cli::unknown_mode option = ::bpkg::cli::unknown_mode::fail,
           ::bpkg::cli::unknown_mode argument = ::bpkg::cli::unknown_mode::stop);

    bool
    parse (::bpkg::cli::scanner&,
           ::bpkg::cli::unknown_mode option = ::bpkg::cli::unknown_mode::fail,
           ::bpkg::cli::unknown_mode argument = ::bpkg::cli::unknown_mode::stop);

    // Merge options from the specified instance appending/overriding
    // them as if they appeared after options in this instance.
    //
    void
    merge (const help_options&);

    // Option accessors.
    //
    // Print usage information.
    //
    static ::bpkg::cli::usage_para
    print_usage (::std::ostream&,
                 ::bpkg::cli::usage_para = ::bpkg::cli::usage_para::none);

    // Implementation details.
    //
    protected:
    bool
    _parse (const char*, ::bpkg::cli::scanner&);

    private:
    bool
    _parse (::bpkg::cli::scanner&,
            ::bpkg::cli::unknown_mode option,
            ::bpkg::cli::unknown_mode argument);

    public:
  };
}

// Print page usage information.
//
namespace bpkg
{
  ::bpkg::cli::usage_para
  print_bpkg_help_usage (::std::ostream&,
                         ::bpkg::cli::usage_para = ::bpkg::cli::usage_para::none);
}

#include <bpkg/help-options.ixx>

// Begin epilogue.
//
//
// End epilogue.

#endif // BPKG_HELP_OPTIONS_HXX
