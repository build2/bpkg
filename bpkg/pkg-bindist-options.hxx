// -*- C++ -*-
//
// This file was generated by CLI, a command line interface
// compiler for C++.
//

#ifndef BPKG_PKG_BINDIST_OPTIONS_HXX
#define BPKG_PKG_BINDIST_OPTIONS_HXX

// Begin prologue.
//
//
// End prologue.

#include <map>

#include <bpkg/configuration-options.hxx>

namespace bpkg
{
  class pkg_bindist_common_options: public ::bpkg::configuration_options
  {
    public:
    pkg_bindist_common_options ();

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
    merge (const pkg_bindist_common_options&);

    // Option accessors.
    //
    const string&
    distribution () const;

    bool
    distribution_specified () const;

    const string&
    architecture () const;

    bool
    architecture_specified () const;

    const strings&
    recursive () const;

    bool
    recursive_specified () const;

    const bool&
    private_ () const;

    const dir_path&
    output_root () const;

    bool
    output_root_specified () const;

    const bool&
    wipe_output () const;

    const bool&
    keep_output () const;

    const bool&
    allow_dependent_config () const;

    const string&
    os_release_id () const;

    bool
    os_release_id_specified () const;

    const string&
    os_release_version_id () const;

    bool
    os_release_version_id_specified () const;

    const string&
    os_release_name () const;

    bool
    os_release_name_specified () const;

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
    string distribution_;
    bool distribution_specified_;
    string architecture_;
    bool architecture_specified_;
    strings recursive_;
    bool recursive_specified_;
    bool private__;
    dir_path output_root_;
    bool output_root_specified_;
    bool wipe_output_;
    bool keep_output_;
    bool allow_dependent_config_;
    string os_release_id_;
    bool os_release_id_specified_;
    string os_release_version_id_;
    bool os_release_version_id_specified_;
    string os_release_name_;
    bool os_release_name_specified_;
  };

  class pkg_bindist_debian_options
  {
    public:
    pkg_bindist_debian_options ();

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
    merge (const pkg_bindist_debian_options&);

    // Option accessors.
    //
    const bool&
    debian_prepare_only () const;

    const string&
    debian_buildflags () const;

    bool
    debian_buildflags_specified () const;

    const strings&
    debian_maint_option () const;

    bool
    debian_maint_option_specified () const;

    const strings&
    debian_build_option () const;

    bool
    debian_build_option_specified () const;

    const string&
    debian_build_meta () const;

    bool
    debian_build_meta_specified () const;

    const string&
    debian_section () const;

    bool
    debian_section_specified () const;

    const string&
    debian_priority () const;

    bool
    debian_priority_specified () const;

    const string&
    debian_maintainer () const;

    bool
    debian_maintainer_specified () const;

    const string&
    debian_architecture () const;

    bool
    debian_architecture_specified () const;

    const string&
    debian_main_langdep () const;

    bool
    debian_main_langdep_specified () const;

    const string&
    debian_dev_langdep () const;

    bool
    debian_dev_langdep_specified () const;

    const string&
    debian_main_extradep () const;

    bool
    debian_main_extradep_specified () const;

    const string&
    debian_dev_extradep () const;

    bool
    debian_dev_extradep_specified () const;

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
    bool debian_prepare_only_;
    string debian_buildflags_;
    bool debian_buildflags_specified_;
    strings debian_maint_option_;
    bool debian_maint_option_specified_;
    strings debian_build_option_;
    bool debian_build_option_specified_;
    string debian_build_meta_;
    bool debian_build_meta_specified_;
    string debian_section_;
    bool debian_section_specified_;
    string debian_priority_;
    bool debian_priority_specified_;
    string debian_maintainer_;
    bool debian_maintainer_specified_;
    string debian_architecture_;
    bool debian_architecture_specified_;
    string debian_main_langdep_;
    bool debian_main_langdep_specified_;
    string debian_dev_langdep_;
    bool debian_dev_langdep_specified_;
    string debian_main_extradep_;
    bool debian_main_extradep_specified_;
    string debian_dev_extradep_;
    bool debian_dev_extradep_specified_;
  };

  class pkg_bindist_fedora_options
  {
    public:
    pkg_bindist_fedora_options ();

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
    merge (const pkg_bindist_fedora_options&);

    // Option accessors.
    //
    const bool&
    fedora_prepare_only () const;

    const string&
    fedora_buildflags () const;

    bool
    fedora_buildflags_specified () const;

    const strings&
    fedora_build_option () const;

    bool
    fedora_build_option_specified () const;

    const strings&
    fedora_query_option () const;

    bool
    fedora_query_option_specified () const;

    const string&
    fedora_dist_tag () const;

    bool
    fedora_dist_tag_specified () const;

    const string&
    fedora_packager () const;

    bool
    fedora_packager_specified () const;

    const string&
    fedora_build_arch () const;

    bool
    fedora_build_arch_specified () const;

    const strings&
    fedora_main_langreq () const;

    bool
    fedora_main_langreq_specified () const;

    const strings&
    fedora_devel_langreq () const;

    bool
    fedora_devel_langreq_specified () const;

    const strings&
    fedora_stat_langreq () const;

    bool
    fedora_stat_langreq_specified () const;

    const strings&
    fedora_main_extrareq () const;

    bool
    fedora_main_extrareq_specified () const;

    const strings&
    fedora_devel_extrareq () const;

    bool
    fedora_devel_extrareq_specified () const;

    const strings&
    fedora_stat_extrareq () const;

    bool
    fedora_stat_extrareq_specified () const;

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
    bool fedora_prepare_only_;
    string fedora_buildflags_;
    bool fedora_buildflags_specified_;
    strings fedora_build_option_;
    bool fedora_build_option_specified_;
    strings fedora_query_option_;
    bool fedora_query_option_specified_;
    string fedora_dist_tag_;
    bool fedora_dist_tag_specified_;
    string fedora_packager_;
    bool fedora_packager_specified_;
    string fedora_build_arch_;
    bool fedora_build_arch_specified_;
    strings fedora_main_langreq_;
    bool fedora_main_langreq_specified_;
    strings fedora_devel_langreq_;
    bool fedora_devel_langreq_specified_;
    strings fedora_stat_langreq_;
    bool fedora_stat_langreq_specified_;
    strings fedora_main_extrareq_;
    bool fedora_main_extrareq_specified_;
    strings fedora_devel_extrareq_;
    bool fedora_devel_extrareq_specified_;
    strings fedora_stat_extrareq_;
    bool fedora_stat_extrareq_specified_;
  };

  class pkg_bindist_archive_options
  {
    public:
    pkg_bindist_archive_options ();

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
    merge (const pkg_bindist_archive_options&);

    // Option accessors.
    //
    const bool&
    archive_prepare_only () const;

    const strings&
    archive_type () const;

    bool
    archive_type_specified () const;

    const std::multimap<string, string>&
    archive_lang () const;

    bool
    archive_lang_specified () const;

    const std::multimap<string, string>&
    archive_lang_impl () const;

    bool
    archive_lang_impl_specified () const;

    const bool&
    archive_no_cpu () const;

    const bool&
    archive_no_os () const;

    const string&
    archive_build_meta () const;

    bool
    archive_build_meta_specified () const;

    const dir_path&
    archive_install_root () const;

    bool
    archive_install_root_specified () const;

    const bool&
    archive_install_config () const;

    const std::map<string, string>&
    archive_split () const;

    bool
    archive_split_specified () const;

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
    bool archive_prepare_only_;
    strings archive_type_;
    bool archive_type_specified_;
    std::multimap<string, string> archive_lang_;
    bool archive_lang_specified_;
    std::multimap<string, string> archive_lang_impl_;
    bool archive_lang_impl_specified_;
    bool archive_no_cpu_;
    bool archive_no_os_;
    string archive_build_meta_;
    bool archive_build_meta_specified_;
    dir_path archive_install_root_;
    bool archive_install_root_specified_;
    bool archive_install_config_;
    std::map<string, string> archive_split_;
    bool archive_split_specified_;
  };

  class pkg_bindist_options: public ::bpkg::pkg_bindist_common_options,
    public ::bpkg::pkg_bindist_debian_options,
    public ::bpkg::pkg_bindist_fedora_options,
    public ::bpkg::pkg_bindist_archive_options
  {
    public:
    pkg_bindist_options ();

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
    merge (const pkg_bindist_options&);

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
  print_bpkg_pkg_bindist_usage (::std::ostream&,
                                ::bpkg::cli::usage_para = ::bpkg::cli::usage_para::none);
}

#include <bpkg/pkg-bindist-options.ixx>

// Begin epilogue.
//
//
// End epilogue.

#endif // BPKG_PKG_BINDIST_OPTIONS_HXX
