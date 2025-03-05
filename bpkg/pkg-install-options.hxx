// -*- C++ -*-
//
// This file was generated by CLI, a command line interface
// compiler for C++.
//

#ifndef BPKG_PKG_INSTALL_OPTIONS_HXX
#define BPKG_PKG_INSTALL_OPTIONS_HXX

// Begin prologue.
//
//
// End prologue.

#include <bpkg/configuration-options.hxx>

namespace bpkg
{
  class pkg_install_options: public ::bpkg::configuration_options
  {
    public:
    pkg_install_options ();

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
    merge (const pkg_install_options&);

    // Option accessors.
    //
    const bool&
    all () const;

    const strings&
    all_pattern () const;

    bool
    all_pattern_specified () const;

    const bool&
    immediate () const;

    const bool&
    recursive () const;

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
    bool all_;
    strings all_pattern_;
    bool all_pattern_specified_;
    bool immediate_;
    bool recursive_;
  };
}

// Print page usage information.
//
namespace bpkg
{
  ::bpkg::cli::usage_para
  print_bpkg_pkg_install_usage (::std::ostream&,
                                ::bpkg::cli::usage_para = ::bpkg::cli::usage_para::none);
}

#include <bpkg/pkg-install-options.ixx>

// Begin epilogue.
//
//
// End epilogue.

#endif // BPKG_PKG_INSTALL_OPTIONS_HXX
