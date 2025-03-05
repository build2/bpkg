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

#include <bpkg/pkg-drop-options.hxx>

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
  // pkg_drop_options
  //

  pkg_drop_options::
  pkg_drop_options ()
  : all_ (),
    all_pattern_ (),
    all_pattern_specified_ (false),
    yes_ (),
    no_ (),
    keep_unused_ (),
    drop_dependent_ (),
    keep_dependent_ (),
    dependent_exit_ (),
    dependent_exit_specified_ (false),
    disfigure_only_ (),
    print_only_ (),
    plan_ (),
    plan_specified_ (false)
  {
  }

  bool pkg_drop_options::
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

  bool pkg_drop_options::
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

  bool pkg_drop_options::
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

  bool pkg_drop_options::
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

  bool pkg_drop_options::
  parse (::bpkg::cli::scanner& s,
         ::bpkg::cli::unknown_mode opt,
         ::bpkg::cli::unknown_mode arg)
  {
    bool r = _parse (s, opt, arg);
    return r;
  }

  void pkg_drop_options::
  merge (const pkg_drop_options& a)
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

    if (a.all_pattern_specified_)
    {
      ::bpkg::cli::parser< strings>::merge (
        this->all_pattern_, a.all_pattern_);
      this->all_pattern_specified_ = true;
    }

    if (a.yes_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->yes_, a.yes_);
    }

    if (a.no_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->no_, a.no_);
    }

    if (a.keep_unused_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->keep_unused_, a.keep_unused_);
    }

    if (a.drop_dependent_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->drop_dependent_, a.drop_dependent_);
    }

    if (a.keep_dependent_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->keep_dependent_, a.keep_dependent_);
    }

    if (a.dependent_exit_specified_)
    {
      ::bpkg::cli::parser< uint16_t>::merge (
        this->dependent_exit_, a.dependent_exit_);
      this->dependent_exit_specified_ = true;
    }

    if (a.disfigure_only_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->disfigure_only_, a.disfigure_only_);
    }

    if (a.print_only_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->print_only_, a.print_only_);
    }

    if (a.plan_specified_)
    {
      ::bpkg::cli::parser< string>::merge (
        this->plan_, a.plan_);
      this->plan_specified_ = true;
    }
  }

  ::bpkg::cli::usage_para pkg_drop_options::
  print_usage (::std::ostream& os, ::bpkg::cli::usage_para p)
  {
    CLI_POTENTIALLY_UNUSED (os);

    if (p != ::bpkg::cli::usage_para::none)
      os << ::std::endl;

    os << "\033[1mPKG-DROP OPTIONS\033[0m" << ::std::endl;

    os << std::endl
       << "\033[1m--all\033[0m|\033[1m-a\033[0m                  Drop all held packages." << ::std::endl;

    os << std::endl
       << "\033[1m--all-pattern\033[0m \033[4mpattern\033[0m     Drop held packages that match the specified wildcard" << ::std::endl
       << "                          pattern. Repeat this option to match multiple" << ::std::endl
       << "                          patterns. Note that you may need to quote the pattern" << ::std::endl
       << "                          to prevent expansion by your shell." << ::std::endl;

    os << std::endl
       << "\033[1m--yes\033[0m|\033[1m-y\033[0m                  Assume the answer to all prompts is \033[1myes\033[0m. Note that" << ::std::endl
       << "                          this option does not apply to the dropping of" << ::std::endl
       << "                          dependents; use \033[1m--drop-dependent\033[0m for that." << ::std::endl;

    os << std::endl
       << "\033[1m--no\033[0m|\033[1m-n\033[0m                   Assume the answer to all prompts is \033[1mno\033[0m. Only makes" << ::std::endl
       << "                          sense together with \033[1m--print-only|-p\033[0m." << ::std::endl;

    os << std::endl
       << "\033[1m--keep-unused\033[0m|\033[1m-K\033[0m          Don't drop dependency packages that were" << ::std::endl
       << "                          automatically built but will no longer be used." << ::std::endl;

    os << std::endl
       << "\033[1m--drop-dependent\033[0m|\033[1m-D\033[0m       Don't warn about or ask for confirmation if dropping" << ::std::endl
       << "                          dependent packages." << ::std::endl;

    os << std::endl
       << "\033[1m--keep-dependent\033[0m          Issue an error if attempting to drop dependent" << ::std::endl
       << "                          packages." << ::std::endl;

    os << std::endl
       << "\033[1m--dependent-exit\033[0m \033[4mcode\033[0m     Silently exit with the specified error code if" << ::std::endl
       << "                          attempting to drop dependent packages." << ::std::endl;

    os << std::endl
       << "\033[1m--disfigure-only\033[0m          Disfigure all the packages but don't purge." << ::std::endl;

    os << std::endl
       << "\033[1m--print-only\033[0m|\033[1m-p\033[0m           Print to \033[1mstdout\033[0m what would be done without actually" << ::std::endl
       << "                          doing anything." << ::std::endl;

    os << std::endl
       << "\033[1m--plan\033[0m \033[4mheader\033[0m             Print the plan (even if \033[1m--yes\033[0m is specified) and start" << ::std::endl
       << "                          it with the \033[4mheader\033[0m line (unless it is empty)." << ::std::endl;

    p = ::bpkg::cli::usage_para::option;

    // configuration_options base
    //
    p = ::bpkg::configuration_options::print_usage (os, p);

    return p;
  }

  typedef
  std::map<std::string, void (*) (pkg_drop_options&, ::bpkg::cli::scanner&)>
  _cli_pkg_drop_options_map;

  static _cli_pkg_drop_options_map _cli_pkg_drop_options_map_;

  struct _cli_pkg_drop_options_map_init
  {
    _cli_pkg_drop_options_map_init ()
    {
      _cli_pkg_drop_options_map_["--all"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::all_ >;
      _cli_pkg_drop_options_map_["-a"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::all_ >;
      _cli_pkg_drop_options_map_["--all-pattern"] =
      &::bpkg::cli::thunk< pkg_drop_options, strings, &pkg_drop_options::all_pattern_,
        &pkg_drop_options::all_pattern_specified_ >;
      _cli_pkg_drop_options_map_["--yes"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::yes_ >;
      _cli_pkg_drop_options_map_["-y"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::yes_ >;
      _cli_pkg_drop_options_map_["--no"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::no_ >;
      _cli_pkg_drop_options_map_["-n"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::no_ >;
      _cli_pkg_drop_options_map_["--keep-unused"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::keep_unused_ >;
      _cli_pkg_drop_options_map_["-K"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::keep_unused_ >;
      _cli_pkg_drop_options_map_["--drop-dependent"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::drop_dependent_ >;
      _cli_pkg_drop_options_map_["-D"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::drop_dependent_ >;
      _cli_pkg_drop_options_map_["--keep-dependent"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::keep_dependent_ >;
      _cli_pkg_drop_options_map_["--dependent-exit"] =
      &::bpkg::cli::thunk< pkg_drop_options, uint16_t, &pkg_drop_options::dependent_exit_,
        &pkg_drop_options::dependent_exit_specified_ >;
      _cli_pkg_drop_options_map_["--disfigure-only"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::disfigure_only_ >;
      _cli_pkg_drop_options_map_["--print-only"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::print_only_ >;
      _cli_pkg_drop_options_map_["-p"] =
      &::bpkg::cli::thunk< pkg_drop_options, &pkg_drop_options::print_only_ >;
      _cli_pkg_drop_options_map_["--plan"] =
      &::bpkg::cli::thunk< pkg_drop_options, string, &pkg_drop_options::plan_,
        &pkg_drop_options::plan_specified_ >;
    }
  };

  static _cli_pkg_drop_options_map_init _cli_pkg_drop_options_map_init_;

  bool pkg_drop_options::
  _parse (const char* o, ::bpkg::cli::scanner& s)
  {
    _cli_pkg_drop_options_map::const_iterator i (_cli_pkg_drop_options_map_.find (o));

    if (i != _cli_pkg_drop_options_map_.end ())
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

  bool pkg_drop_options::
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
  print_bpkg_pkg_drop_usage (::std::ostream& os, ::bpkg::cli::usage_para p)
  {
    CLI_POTENTIALLY_UNUSED (os);

    if (p != ::bpkg::cli::usage_para::none)
      os << ::std::endl;

    os << "\033[1mSYNOPSIS\033[0m" << ::std::endl
       << ::std::endl
       << "\033[1mbpkg pkg-drop\033[0m|\033[1mdrop\033[0m [\033[4moptions\033[0m] <pkg>..." << ::std::endl
       << "\033[1mbpkg pkg-drop\033[0m|\033[1mdrop\033[0m [\033[4moptions\033[0m] \033[1m--all\033[0m|\033[1m-a\033[0m" << ::std::endl
       << "\033[1mbpkg pkg-drop\033[0m|\033[1mdrop\033[0m [\033[4moptions\033[0m] (\033[1m--all-pattern\033[0m <pattern>)...\033[0m" << ::std::endl
       << ::std::endl
       << "\033[1mDESCRIPTION\033[0m" << ::std::endl
       << ::std::endl
       << "The \033[1mpkg-drop\033[0m command drops from the configuration the specified packages (the" << ::std::endl
       << "first form), all the held packages (the second form, see \033[1mbpkg-pkg-status(1)\033[0m)," << ::std::endl
       << "or all the held packages that match any of the specified wildcard patterns (the" << ::std::endl
       << "third form). If the packages being dropped still have dependent packages, then" << ::std::endl
       << "those will have to be dropped as well and you will be prompted to confirm. And" << ::std::endl
       << "if the packages being dropped have dependency packages that would otherwise no" << ::std::endl
       << "longer be used, then they will be dropped as well unless the \033[1m--keep-unused\033[0m|\033[1m-K\033[0m\033[0m" << ::std::endl
       << "option is specified." << ::std::endl
       << ::std::endl
       << "The \033[1mpkg-drop\033[0m command also supports several options (described below) that allow" << ::std::endl
       << "you to control the amount of work that will be done." << ::std::endl;

    p = ::bpkg::pkg_drop_options::print_usage (os, ::bpkg::cli::usage_para::text);

    if (p != ::bpkg::cli::usage_para::none)
      os << ::std::endl;

    os << "\033[1mDEFAULT OPTIONS FILES\033[0m" << ::std::endl
       << ::std::endl
       << "See \033[1mbpkg-default-options-files(1)\033[0m for an overview of the default options files." << ::std::endl
       << "For the \033[1mpkg-drop\033[0m command the search start directory is the configuration" << ::std::endl
       << "directory. The following options files are searched for in each directory and," << ::std::endl
       << "if found, loaded in the order listed:" << ::std::endl
       << ::std::endl
       << "bpkg.options" << ::std::endl
       << "bpkg-pkg-drop.options" << ::std::endl
       << ::std::endl
       << "The following \033[1mpkg-drop\033[0m command options cannot be specified in the default" << ::std::endl
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

