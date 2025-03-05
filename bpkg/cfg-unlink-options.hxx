// -*- C++ -*-
//
// This file was generated by CLI, a command line interface
// compiler for C++.
//

#ifndef BPKG_CFG_UNLINK_OPTIONS_HXX
#define BPKG_CFG_UNLINK_OPTIONS_HXX

// Begin prologue.
//
//
// End prologue.

#include <bpkg/configuration-options.hxx>

namespace bpkg
{
  class cfg_unlink_options: public ::bpkg::configuration_options
  {
    public:
    cfg_unlink_options ();

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
    merge (const cfg_unlink_options&);

    // Option accessors.
    //
    const string&
    name () const;

    bool
    name_specified () const;

    const uint64_t&
    id () const;

    bool
    id_specified () const;

    const uuid_type&
    uuid () const;

    bool
    uuid_specified () const;

    const bool&
    dangling () const;

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
    string name_;
    bool name_specified_;
    uint64_t id_;
    bool id_specified_;
    uuid_type uuid_;
    bool uuid_specified_;
    bool dangling_;
  };
}

// Print page usage information.
//
namespace bpkg
{
  ::bpkg::cli::usage_para
  print_bpkg_cfg_unlink_usage (::std::ostream&,
                               ::bpkg::cli::usage_para = ::bpkg::cli::usage_para::none);
}

#include <bpkg/cfg-unlink-options.ixx>

// Begin epilogue.
//
//
// End epilogue.

#endif // BPKG_CFG_UNLINK_OPTIONS_HXX
