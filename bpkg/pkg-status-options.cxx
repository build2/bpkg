// -*- C++ -*-
//
// This file was generated by CLI, a command line interface
// compiler for C++.
//

// Begin prologue.
//
#include <bpkg/types-parsers.hxx>
//
// End prologue.

#include <bpkg/pkg-status-options.hxx>

#include <map>
#include <set>
#include <string>
#include <vector>
#include <utility>
#include <ostream>
#include <sstream>
#include <cstring>

namespace bpkg
{
  namespace cli
  {
    template <typename X>
    struct parser
    {
      static void
      parse (X& x, bool& xs, scanner& s)
      {
        using namespace std;

        const char* o (s.next ());
        if (s.more ())
        {
          string v (s.next ());
          istringstream is (v);
          if (!(is >> x && is.peek () == istringstream::traits_type::eof ()))
            throw invalid_value (o, v);
        }
        else
          throw missing_value (o);

        xs = true;
      }

      static void
      merge (X& b, const X& a)
      {
        b = a;
      }
    };

    template <>
    struct parser<bool>
    {
      static void
      parse (bool& x, bool& xs, scanner& s)
      {
        const char* o (s.next ());

        if (s.more ())
        {
          const char* v (s.next ());

          if (std::strcmp (v, "1")    == 0 ||
              std::strcmp (v, "true") == 0 ||
              std::strcmp (v, "TRUE") == 0 ||
              std::strcmp (v, "True") == 0)
            x = true;
          else if (std::strcmp (v, "0")     == 0 ||
                   std::strcmp (v, "false") == 0 ||
                   std::strcmp (v, "FALSE") == 0 ||
                   std::strcmp (v, "False") == 0)
            x = false;
          else
            throw invalid_value (o, v);
        }
        else
          throw missing_value (o);

        xs = true;
      }

      static void
      merge (bool& b, const bool&)
      {
        b = true;
      }
    };

    template <>
    struct parser<std::string>
    {
      static void
      parse (std::string& x, bool& xs, scanner& s)
      {
        const char* o (s.next ());

        if (s.more ())
          x = s.next ();
        else
          throw missing_value (o);

        xs = true;
      }

      static void
      merge (std::string& b, const std::string& a)
      {
        b = a;
      }
    };

    template <typename X>
    struct parser<std::pair<X, std::size_t> >
    {
      static void
      parse (std::pair<X, std::size_t>& x, bool& xs, scanner& s)
      {
        x.second = s.position ();
        parser<X>::parse (x.first, xs, s);
      }

      static void
      merge (std::pair<X, std::size_t>& b, const std::pair<X, std::size_t>& a)
      {
        b = a;
      }
    };

    template <typename X>
    struct parser<std::vector<X> >
    {
      static void
      parse (std::vector<X>& c, bool& xs, scanner& s)
      {
        X x;
        bool dummy;
        parser<X>::parse (x, dummy, s);
        c.push_back (x);
        xs = true;
      }

      static void
      merge (std::vector<X>& b, const std::vector<X>& a)
      {
        b.insert (b.end (), a.begin (), a.end ());
      }
    };

    template <typename X, typename C>
    struct parser<std::set<X, C> >
    {
      static void
      parse (std::set<X, C>& c, bool& xs, scanner& s)
      {
        X x;
        bool dummy;
        parser<X>::parse (x, dummy, s);
        c.insert (x);
        xs = true;
      }

      static void
      merge (std::set<X, C>& b, const std::set<X, C>& a)
      {
        b.insert (a.begin (), a.end ());
      }
    };

    template <typename K, typename V, typename C>
    struct parser<std::map<K, V, C> >
    {
      static void
      parse (std::map<K, V, C>& m, bool& xs, scanner& s)
      {
        const char* o (s.next ());

        if (s.more ())
        {
          std::size_t pos (s.position ());
          std::string ov (s.next ());
          std::string::size_type p = ov.find ('=');

          K k = K ();
          V v = V ();
          std::string kstr (ov, 0, p);
          std::string vstr (ov, (p != std::string::npos ? p + 1 : ov.size ()));

          int ac (2);
          char* av[] =
          {
            const_cast<char*> (o),
            0
          };

          bool dummy;
          if (!kstr.empty ())
          {
            av[1] = const_cast<char*> (kstr.c_str ());
            argv_scanner s (0, ac, av, false, pos);
            parser<K>::parse (k, dummy, s);
          }

          if (!vstr.empty ())
          {
            av[1] = const_cast<char*> (vstr.c_str ());
            argv_scanner s (0, ac, av, false, pos);
            parser<V>::parse (v, dummy, s);
          }

          m[k] = v;
        }
        else
          throw missing_value (o);

        xs = true;
      }

      static void
      merge (std::map<K, V, C>& b, const std::map<K, V, C>& a)
      {
        for (typename std::map<K, V, C>::const_iterator i (a.begin ()); 
             i != a.end (); 
             ++i)
          b[i->first] = i->second;
      }
    };

    template <typename K, typename V, typename C>
    struct parser<std::multimap<K, V, C> >
    {
      static void
      parse (std::multimap<K, V, C>& m, bool& xs, scanner& s)
      {
        const char* o (s.next ());

        if (s.more ())
        {
          std::size_t pos (s.position ());
          std::string ov (s.next ());
          std::string::size_type p = ov.find ('=');

          K k = K ();
          V v = V ();
          std::string kstr (ov, 0, p);
          std::string vstr (ov, (p != std::string::npos ? p + 1 : ov.size ()));

          int ac (2);
          char* av[] =
          {
            const_cast<char*> (o),
            0
          };

          bool dummy;
          if (!kstr.empty ())
          {
            av[1] = const_cast<char*> (kstr.c_str ());
            argv_scanner s (0, ac, av, false, pos);
            parser<K>::parse (k, dummy, s);
          }

          if (!vstr.empty ())
          {
            av[1] = const_cast<char*> (vstr.c_str ());
            argv_scanner s (0, ac, av, false, pos);
            parser<V>::parse (v, dummy, s);
          }

          m.insert (typename std::multimap<K, V, C>::value_type (k, v));
        }
        else
          throw missing_value (o);

        xs = true;
      }

      static void
      merge (std::multimap<K, V, C>& b, const std::multimap<K, V, C>& a)
      {
        for (typename std::multimap<K, V, C>::const_iterator i (a.begin ()); 
             i != a.end (); 
             ++i)
          b.insert (typename std::multimap<K, V, C>::value_type (i->first,
                                                                 i->second));
      }
    };

    template <typename X, typename T, T X::*M>
    void
    thunk (X& x, scanner& s)
    {
      parser<T>::parse (x.*M, s);
    }

    template <typename X, bool X::*M>
    void
    thunk (X& x, scanner& s)
    {
      s.next ();
      x.*M = true;
    }

    template <typename X, typename T, T X::*M, bool X::*S>
    void
    thunk (X& x, scanner& s)
    {
      parser<T>::parse (x.*M, x.*S, s);
    }
  }
}

#include <map>

namespace bpkg
{
  // pkg_status_options
  //

  pkg_status_options::
  pkg_status_options ()
  : all_ (),
    link_ (),
    immediate_ (),
    recursive_ (),
    old_available_ (),
    constraint_ (),
    system_ (),
    no_hold_ (),
    no_hold_package_ (),
    no_hold_version_ ()
  {
  }

  bool pkg_status_options::
  parse (int& argc,
         char** argv,
         bool erase,
         ::bpkg::cli::unknown_mode opt,
         ::bpkg::cli::unknown_mode arg)
  {
    ::bpkg::cli::argv_scanner s (argc, argv, erase);
    bool r = _parse (s, opt, arg);
    return r;
  }

  bool pkg_status_options::
  parse (int start,
         int& argc,
         char** argv,
         bool erase,
         ::bpkg::cli::unknown_mode opt,
         ::bpkg::cli::unknown_mode arg)
  {
    ::bpkg::cli::argv_scanner s (start, argc, argv, erase);
    bool r = _parse (s, opt, arg);
    return r;
  }

  bool pkg_status_options::
  parse (int& argc,
         char** argv,
         int& end,
         bool erase,
         ::bpkg::cli::unknown_mode opt,
         ::bpkg::cli::unknown_mode arg)
  {
    ::bpkg::cli::argv_scanner s (argc, argv, erase);
    bool r = _parse (s, opt, arg);
    end = s.end ();
    return r;
  }

  bool pkg_status_options::
  parse (int start,
         int& argc,
         char** argv,
         int& end,
         bool erase,
         ::bpkg::cli::unknown_mode opt,
         ::bpkg::cli::unknown_mode arg)
  {
    ::bpkg::cli::argv_scanner s (start, argc, argv, erase);
    bool r = _parse (s, opt, arg);
    end = s.end ();
    return r;
  }

  bool pkg_status_options::
  parse (::bpkg::cli::scanner& s,
         ::bpkg::cli::unknown_mode opt,
         ::bpkg::cli::unknown_mode arg)
  {
    bool r = _parse (s, opt, arg);
    return r;
  }

  void pkg_status_options::
  merge (const pkg_status_options& a)
  {
    CLI_POTENTIALLY_UNUSED (a);

    // configuration_options base
    //
    ::bpkg::configuration_options::merge (a);

    if (a.all_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->all_, a.all_);
    }

    if (a.link_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->link_, a.link_);
    }

    if (a.immediate_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->immediate_, a.immediate_);
    }

    if (a.recursive_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->recursive_, a.recursive_);
    }

    if (a.old_available_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->old_available_, a.old_available_);
    }

    if (a.constraint_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->constraint_, a.constraint_);
    }

    if (a.system_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->system_, a.system_);
    }

    if (a.no_hold_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->no_hold_, a.no_hold_);
    }

    if (a.no_hold_package_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->no_hold_package_, a.no_hold_package_);
    }

    if (a.no_hold_version_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->no_hold_version_, a.no_hold_version_);
    }
  }

  ::bpkg::cli::usage_para pkg_status_options::
  print_usage (::std::ostream& os, ::bpkg::cli::usage_para p)
  {
    CLI_POTENTIALLY_UNUSED (os);

    if (p != ::bpkg::cli::usage_para::none)
      os << ::std::endl;

    os << "\033[1mPKG-STATUS OPTIONS\033[0m" << ::std::endl;

    os << std::endl
       << "\033[1m--all\033[0m|\033[1m-a\033[0m                  Print the status of all the packages, not just held." << ::std::endl;

    os << std::endl
       << "\033[1m--link\033[0m                    Also print the status of held/all packages from" << ::std::endl
       << "                          linked configurations." << ::std::endl;

    os << std::endl
       << "\033[1m--immediate\033[0m|\033[1m-i\033[0m            Also print the status of immediate dependencies." << ::std::endl;

    os << std::endl
       << "\033[1m--recursive\033[0m|\033[1m-r\033[0m            Also print the status of all dependencies," << ::std::endl
       << "                          recursively." << ::std::endl;

    os << std::endl
       << "\033[1m--old-available\033[0m|\033[1m-o\033[0m        Print old available versions." << ::std::endl;

    os << std::endl
       << "\033[1m--constraint\033[0m              Print version constraints for dependencies." << ::std::endl;

    os << std::endl
       << "\033[1m--system\033[0m                  Check the availability of packages from the system." << ::std::endl;

    os << std::endl
       << "\033[1m--no-hold\033[0m                 Don't print the package or version hold status." << ::std::endl;

    os << std::endl
       << "\033[1m--no-hold-package\033[0m         Don't print the package hold status." << ::std::endl;

    os << std::endl
       << "\033[1m--no-hold-version\033[0m         Don't print the version hold status." << ::std::endl;

    p = ::bpkg::cli::usage_para::option;

    // configuration_options base
    //
    p = ::bpkg::configuration_options::print_usage (os, p);

    return p;
  }

  typedef
  std::map<std::string, void (*) (pkg_status_options&, ::bpkg::cli::scanner&)>
  _cli_pkg_status_options_map;

  static _cli_pkg_status_options_map _cli_pkg_status_options_map_;

  struct _cli_pkg_status_options_map_init
  {
    _cli_pkg_status_options_map_init ()
    {
      _cli_pkg_status_options_map_["--all"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::all_ >;
      _cli_pkg_status_options_map_["-a"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::all_ >;
      _cli_pkg_status_options_map_["--link"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::link_ >;
      _cli_pkg_status_options_map_["--immediate"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::immediate_ >;
      _cli_pkg_status_options_map_["-i"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::immediate_ >;
      _cli_pkg_status_options_map_["--recursive"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::recursive_ >;
      _cli_pkg_status_options_map_["-r"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::recursive_ >;
      _cli_pkg_status_options_map_["--old-available"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::old_available_ >;
      _cli_pkg_status_options_map_["-o"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::old_available_ >;
      _cli_pkg_status_options_map_["--constraint"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::constraint_ >;
      _cli_pkg_status_options_map_["--system"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::system_ >;
      _cli_pkg_status_options_map_["--no-hold"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::no_hold_ >;
      _cli_pkg_status_options_map_["--no-hold-package"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::no_hold_package_ >;
      _cli_pkg_status_options_map_["--no-hold-version"] =
      &::bpkg::cli::thunk< pkg_status_options, &pkg_status_options::no_hold_version_ >;
    }
  };

  static _cli_pkg_status_options_map_init _cli_pkg_status_options_map_init_;

  bool pkg_status_options::
  _parse (const char* o, ::bpkg::cli::scanner& s)
  {
    _cli_pkg_status_options_map::const_iterator i (_cli_pkg_status_options_map_.find (o));

    if (i != _cli_pkg_status_options_map_.end ())
    {
      (*(i->second)) (*this, s);
      return true;
    }

    // configuration_options base
    //
    if (::bpkg::configuration_options::_parse (o, s))
      return true;

    return false;
  }

  bool pkg_status_options::
  _parse (::bpkg::cli::scanner& s,
          ::bpkg::cli::unknown_mode opt_mode,
          ::bpkg::cli::unknown_mode arg_mode)
  {
    // Can't skip combined flags (--no-combined-flags).
    //
    assert (opt_mode != ::bpkg::cli::unknown_mode::skip);

    bool r = false;
    bool opt = true;

    while (s.more ())
    {
      const char* o = s.peek ();

      if (std::strcmp (o, "--") == 0)
      {
        opt = false;
      }

      if (opt)
      {
        if (_parse (o, s))
        {
          r = true;
          continue;
        }

        if (std::strncmp (o, "-", 1) == 0 && o[1] != '\0')
        {
          // Handle combined option values.
          //
          std::string co;
          if (const char* v = std::strchr (o, '='))
          {
            co.assign (o, 0, v - o);
            ++v;

            int ac (2);
            char* av[] =
            {
              const_cast<char*> (co.c_str ()),
              const_cast<char*> (v)
            };

            ::bpkg::cli::argv_scanner ns (0, ac, av);

            if (_parse (co.c_str (), ns))
            {
              // Parsed the option but not its value?
              //
              if (ns.end () != 2)
                throw ::bpkg::cli::invalid_value (co, v);

              s.next ();
              r = true;
              continue;
            }
            else
            {
              // Set the unknown option and fall through.
              //
              o = co.c_str ();
            }
          }

          // Handle combined flags.
          //
          char cf[3];
          {
            const char* p = o + 1;
            for (; *p != '\0'; ++p)
            {
              if (!((*p >= 'a' && *p <= 'z') ||
                    (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9')))
                break;
            }

            if (*p == '\0')
            {
              for (p = o + 1; *p != '\0'; ++p)
              {
                std::strcpy (cf, "-");
                cf[1] = *p;
                cf[2] = '\0';

                int ac (1);
                char* av[] =
                {
                  cf
                };

                ::bpkg::cli::argv_scanner ns (0, ac, av);

                if (!_parse (cf, ns))
                  break;
              }

              if (*p == '\0')
              {
                // All handled.
                //
                s.next ();
                r = true;
                continue;
              }
              else
              {
                // Set the unknown option and fall through.
                //
                o = cf;
              }
            }
          }

          switch (opt_mode)
          {
            case ::bpkg::cli::unknown_mode::skip:
            {
              s.skip ();
              r = true;
              continue;
            }
            case ::bpkg::cli::unknown_mode::stop:
            {
              break;
            }
            case ::bpkg::cli::unknown_mode::fail:
            {
              throw ::bpkg::cli::unknown_option (o);
            }
          }

          break;
        }
      }

      switch (arg_mode)
      {
        case ::bpkg::cli::unknown_mode::skip:
        {
          s.skip ();
          r = true;
          continue;
        }
        case ::bpkg::cli::unknown_mode::stop:
        {
          break;
        }
        case ::bpkg::cli::unknown_mode::fail:
        {
          throw ::bpkg::cli::unknown_argument (o);
        }
      }

      break;
    }

    return r;
  }
}

namespace bpkg
{
  ::bpkg::cli::usage_para
  print_bpkg_pkg_status_usage (::std::ostream& os, ::bpkg::cli::usage_para p)
  {
    CLI_POTENTIALLY_UNUSED (os);

    if (p != ::bpkg::cli::usage_para::none)
      os << ::std::endl;

    os << "\033[1mSYNOPSIS\033[0m" << ::std::endl
       << ::std::endl
       << "\033[1mbpkg pkg-status\033[0m|\033[1mstatus\033[0m [\033[4moptions\033[0m] [\033[4mpkg\033[0m[\033[1m/\033[0m\033[4mver\033[0m]...]\033[0m" << ::std::endl
       << ::std::endl
       << "\033[1mDESCRIPTION\033[0m" << ::std::endl
       << ::std::endl
       << "The \033[1mpkg-status\033[0m command prints the status of the specified packages or, if \033[4mver\033[0m" << ::std::endl
       << "is specified, package versions. If no packages were specified, then \033[1mpkg-status\033[0m" << ::std::endl
       << "prints the status of all the held packages (which are the packages that were" << ::std::endl
       << "explicitly built; see \033[1mbpkg-pkg-build(1)\033[0m). The latter mode can be modified to" << ::std::endl
       << "print the status of all the packages by specifying the \033[1m--all\033[0m|\033[1m-a\033[0m\033[0m option." << ::std::endl
       << "Additionally, the status of immediate or all dependencies of the above packages" << ::std::endl
       << "can be printed by specifying the \033[1m--immediate\033[0m|\033[1m-i\033[0m\033[0m or \033[1m--recursive\033[0m|\033[1m-r\033[0m\033[0m options," << ::std::endl
       << "respectively. Note that the status is written to \033[1mstdout\033[0m, not \033[1mstderr\033[0m." << ::std::endl
       << ::std::endl
       << "The default output format (see the \033[1m--stdout-format\033[0m common option) is regular" << ::std::endl
       << "with components separated with spaces. Each line starts with the package name" << ::std::endl
       << "followed by one of the status words listed below. Some of them can be" << ::std::endl
       << "optionally followed by '\033[1m,\033[0m' (no spaces) and a sub-status word. Lines" << ::std::endl
       << "corresponding to dependencies from linked configurations will additionally" << ::std::endl
       << "mention the configuration directory in square brackets after the package name." << ::std::endl
       << ::std::endl
       << "\033[1munknown\033[0m" << ::std::endl
       << "    Package is not part of the configuration nor available from any of the" << ::std::endl
       << "    repositories." << ::std::endl
       << "\033[1mavailable\033[0m" << ::std::endl
       << "    Package is not part of the configuration but is available from one of the" << ::std::endl
       << "    repositories." << ::std::endl
       << "\033[1mfetched\033[0m" << ::std::endl
       << "    Package is part of the configuration and is fetched." << ::std::endl
       << "\033[1munpacked\033[0m" << ::std::endl
       << "    Package is part of the configuration and is unpacked." << ::std::endl
       << "\033[1mconfigured\033[0m" << ::std::endl
       << "    Package is part of the configuration and is configured. May be followed by" << ::std::endl
       << "    the \033[1msystem\033[0m sub-status indicating a package coming from the system. The" << ::std::endl
       << "    version of such a system package (described below) may be the special '\033[1m*\033[0m'" << ::std::endl
       << "    value indicating a wildcard version." << ::std::endl
       << "\033[1mbroken\033[0m" << ::std::endl
       << "    Package is part of the configuration and is broken (broken packages can" << ::std::endl
       << "    only be purged; see \033[1mbpkg-pkg-purge(1)\033[0m)." << ::std::endl
       << ::std::endl
       << "If only the package name was specified without the package version, then the" << ::std::endl
       << "\033[1mavailable\033[0m status word is followed by the list of available versions. Versions" << ::std::endl
       << "that are only available for up/down-grading are printed in '\033[1m[]\033[0m' (such version" << ::std::endl
       << "are only available as dependencies from prerequisite repositories of other" << ::std::endl
       << "repositories). If the \033[1m--system\033[0m option is specified, then the last version in" << ::std::endl
       << "this list may have the \033[1msys:\033[0m prefix indicating an available system version. Such" << ::std::endl
       << "a system version may be the special '\033[1m?\033[0m' value indicating that a package may or" << ::std::endl
       << "may not be available from the system and that its version is unknown." << ::std::endl
       << ::std::endl
       << "The \033[1mfetched\033[0m, \033[1munpacked\033[0m, \033[1mconfigured\033[0m, and \033[1mbroken\033[0m status words are followed by the" << ::std::endl
       << "version of the package. If the package version was specified, then the \033[1munknown\033[0m" << ::std::endl
       << "status word is also followed by the version." << ::std::endl
       << ::std::endl
       << "If the status is \033[1mfetched\033[0m, \033[1munpacked\033[0m, \033[1mconfigured\033[0m, or \033[1mbroken\033[0m and newer versions" << ::std::endl
       << "are available, then the package version is followed by the \033[1mavailable\033[0m status" << ::std::endl
       << "word and the list of newer versions. To instead see a list of all versions," << ::std::endl
       << "including the older ones, specify the \033[1m--old-available\033[0m|\033[1m-o\033[0m\033[0m option. In this case" << ::std::endl
       << "the currently selected version is printed in '\033[1m()\033[0m'." << ::std::endl
       << ::std::endl
       << "If the package name was specified with the version, then only the status (such" << ::std::endl
       << "as, \033[1mconfigured\033[0m, \033[1mavailable\033[0m, etc.) of this version is considered." << ::std::endl
       << ::std::endl
       << "If a package is being held, then its name is printed prefixed with '\033[1m!\033[0m'." << ::std::endl
       << "Similarly, if a package version is being held, then the version is printed" << ::std::endl
       << "prefixed with '\033[1m!\033[0m'. Held packages and held versions were selected by the user" << ::std::endl
       << "and are not automatically dropped and upgraded, respectively." << ::std::endl
       << ::std::endl
       << "Below are some examples, assuming the configuration has \033[1mlibfoo\033[0m \033[1m1.0.0\033[0m configured" << ::std::endl
       << "and held (both package and version) as well as \033[1mlibfoo\033[0m \033[1m1.1.0\033[0m and \033[1m1.1.1\033[0m available" << ::std::endl
       << "from source and \033[1m1.1.0\033[0m from the system." << ::std::endl
       << ::std::endl
       << "bpkg status libbar" << ::std::endl
       << "libbar unknown" << ::std::endl
       << ::std::endl
       << "bpkg status libbar/1.0.0" << ::std::endl
       << "libbar unknown 1.0.0" << ::std::endl
       << ::std::endl
       << "bpkg status libfoo/1.0.0" << ::std::endl
       << "!libfoo configured !1.0.0" << ::std::endl
       << ::std::endl
       << "bpkg status libfoo/1.1.0" << ::std::endl
       << "libfoo available 1.1.0" << ::std::endl
       << ::std::endl
       << "bpkg status --system libfoo/1.1.0" << ::std::endl
       << "libfoo available 1.1.0 sys:1.1.0" << ::std::endl
       << ::std::endl
       << "bpkg status libfoo" << ::std::endl
       << "!libfoo configured !1.0.0 available 1.1.0 1.1.1" << ::std::endl
       << ::std::endl
       << "bpkg status libfoo/1.1.1 libbar" << ::std::endl
       << "libfoo available 1.1.1" << ::std::endl
       << "libbar unknown" << ::std::endl
       << ::std::endl
       << "Assuming now that we dropped \033[1mlibfoo\033[0m from the configuration:" << ::std::endl
       << ::std::endl
       << "bpkg status libfoo/1.0.0" << ::std::endl
       << "libfoo unknown 1.0.0" << ::std::endl
       << ::std::endl
       << "bpkg status libfoo" << ::std::endl
       << "libfoo available 1.1.0 1.1.1" << ::std::endl
       << ::std::endl
       << "And assuming now that we built \033[1mlibfoo\033[0m as a system package with the wildcard" << ::std::endl
       << "version:" << ::std::endl
       << ::std::endl
       << "bpkg status libfoo" << ::std::endl
       << "!libfoo configured,system !* available 1.1.0 1.1.1" << ::std::endl
       << ::std::endl
       << "Another example of the status output this time including dependencies:" << ::std::endl
       << ::std::endl
       << "bpkg status -r libbaz" << ::std::endl
       << "!libbaz configured 1.0.0" << ::std::endl
       << "  libfoo configured 1.0.0" << ::std::endl
       << "    bison [.bpkg/host/] configured 1.0.0" << ::std::endl
       << "  libbar configured 2.0.0" << ::std::endl
       << ::std::endl
       << "If the output format is \033[1mjson\033[0m, then the output is a JSON array of objects which" << ::std::endl
       << "are the serialized representation of the following C++ \033[1mstruct\033[0m \033[1mpackage_status\033[0m:" << ::std::endl
       << ::std::endl
       << "struct available_version" << ::std::endl
       << "{" << ::std::endl
       << "  string version;" << ::std::endl
       << "  bool   system;" << ::std::endl
       << "  bool   dependency;" << ::std::endl
       << "};" << ::std::endl
       << ::std::endl
       << "struct package_status" << ::std::endl
       << "{" << ::std::endl
       << "  string                    name;" << ::std::endl
       << "  optional<string>          configuration;" << ::std::endl
       << "  optional<string>          constraint;" << ::std::endl
       << "  string                    status;" << ::std::endl
       << "  optional<string>          sub_status;" << ::std::endl
       << "  optional<string>          version;" << ::std::endl
       << "  bool                      hold_package;" << ::std::endl
       << "  bool                      hold_version;" << ::std::endl
       << "  vector<available_version> available_versions;" << ::std::endl
       << "  vector<package_status>    dependencies;" << ::std::endl
       << "};" << ::std::endl
       << ::std::endl
       << "For example:" << ::std::endl
       << ::std::endl
       << "[" << ::std::endl
       << "  {" << ::std::endl
       << "    \"name\": \"hello\"," << ::std::endl
       << "    \"status\": \"configured\"," << ::std::endl
       << "    \"version\": \"1.0.0\"," << ::std::endl
       << "    \"hold_package\": true," << ::std::endl
       << "    \"available_versions\": [" << ::std::endl
       << "      {" << ::std::endl
       << "        \"version\": \"1.0.1\"" << ::std::endl
       << "      }," << ::std::endl
       << "      {" << ::std::endl
       << "        \"version\": \"2.0.0\"" << ::std::endl
       << "      }" << ::std::endl
       << "    ]," << ::std::endl
       << "    \"dependencies\": [" << ::std::endl
       << "      {" << ::std::endl
       << "        \"name\": \"libhello\"," << ::std::endl
       << "        \"status\": \"configured\"," << ::std::endl
       << "        \"version\": \"1.0.2\"," << ::std::endl
       << "      }" << ::std::endl
       << "    ]" << ::std::endl
       << "  }" << ::std::endl
       << "]" << ::std::endl
       << ::std::endl
       << "See the JSON OUTPUT section in \033[1mbpkg-common-options(1)\033[0m for details on the" << ::std::endl
       << "overall properties of this format and the semantics of the \033[1mstruct\033[0m" << ::std::endl
       << "serialization." << ::std::endl
       << ::std::endl
       << "In \033[1mpackage_status\033[0m, the \033[1mconfiguration\033[0m member contains the absolute directory of" << ::std::endl
       << "a linked configuration if this package resides in a linked configuration. The" << ::std::endl
       << "\033[1mconstraint\033[0m member is present only if the \033[1m--constraint\033[0m option is specified. The" << ::std::endl
       << "\033[1mversion\033[0m member is absent if the \033[1mstatus\033[0m member is \033[1munknown\033[0m or \033[1mavailable\033[0m and no" << ::std::endl
       << "package version is specified on the command line. If the \033[1msub_status\033[0m member is" << ::std::endl
       << "\033[1msystem\033[0m, then the \033[1mversion\033[0m member can be special \033[1m*\033[0m. The \033[1mdependencies\033[0m member is" << ::std::endl
       << "present only if the \033[1m--immediate|-i\033[0m or \033[1m--recursive|-r\033[0m options are specified." << ::std::endl
       << ::std::endl
       << "In \033[1mavailable_version\033[0m, if the \033[1msystem\033[0m member is \033[1mtrue\033[0m, then this version is" << ::std::endl
       << "available from the system, in which case the \033[1mversion\033[0m member can be special \033[1m?\033[0m or" << ::std::endl
       << "\033[1m*\033[0m. If the \033[1mdependency\033[0m member is \033[1mtrue\033[0m, then this version is only available as a" << ::std::endl
       << "dependency from prerequisite repositories of other repositories." << ::std::endl;

    p = ::bpkg::pkg_status_options::print_usage (os, ::bpkg::cli::usage_para::text);

    if (p != ::bpkg::cli::usage_para::none)
      os << ::std::endl;

    os << "\033[1mDEFAULT OPTIONS FILES\033[0m" << ::std::endl
       << ::std::endl
       << "See \033[1mbpkg-default-options-files(1)\033[0m for an overview of the default options files." << ::std::endl
       << "For the \033[1mpkg-status\033[0m command the search start directory is the configuration" << ::std::endl
       << "directory. The following options files are searched for in each directory and," << ::std::endl
       << "if found, loaded in the order listed:" << ::std::endl
       << ::std::endl
       << "bpkg.options" << ::std::endl
       << "bpkg-pkg-status.options" << ::std::endl
       << ::std::endl
       << "The following \033[1mpkg-status\033[0m command options cannot be specified in the default" << ::std::endl
       << "options files:" << ::std::endl
       << ::std::endl
       << "--directory|-d" << ::std::endl;

    p = ::bpkg::cli::usage_para::text;

    return p;
  }
}

// Begin epilogue.
//
//
// End epilogue.

