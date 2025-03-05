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

#include <bpkg/rep-create-options.hxx>

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
  // rep_create_options
  //

  rep_create_options::
  rep_create_options ()
  : ignore_unknown_ (),
    min_bpkg_version_ (),
    min_bpkg_version_specified_ (false),
    key_ (),
    key_specified_ (false)
  {
  }

  bool rep_create_options::
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

  bool rep_create_options::
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

  bool rep_create_options::
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

  bool rep_create_options::
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

  bool rep_create_options::
  parse (::bpkg::cli::scanner& s,
         ::bpkg::cli::unknown_mode opt,
         ::bpkg::cli::unknown_mode arg)
  {
    bool r = _parse (s, opt, arg);
    return r;
  }

  void rep_create_options::
  merge (const rep_create_options& a)
  {
    CLI_POTENTIALLY_UNUSED (a);

    // common_options base
    //
    ::bpkg::common_options::merge (a);

    if (a.ignore_unknown_)
    {
      ::bpkg::cli::parser< bool>::merge (
        this->ignore_unknown_, a.ignore_unknown_);
    }

    if (a.min_bpkg_version_specified_)
    {
      ::bpkg::cli::parser< butl::standard_version>::merge (
        this->min_bpkg_version_, a.min_bpkg_version_);
      this->min_bpkg_version_specified_ = true;
    }

    if (a.key_specified_)
    {
      ::bpkg::cli::parser< string>::merge (
        this->key_, a.key_);
      this->key_specified_ = true;
    }
  }

  ::bpkg::cli::usage_para rep_create_options::
  print_usage (::std::ostream& os, ::bpkg::cli::usage_para p)
  {
    CLI_POTENTIALLY_UNUSED (os);

    if (p != ::bpkg::cli::usage_para::none)
      os << ::std::endl;

    os << "\033[1mREP-CREATE OPTIONS\033[0m" << ::std::endl;

    os << std::endl
       << "\033[1m--ignore-unknown\033[0m          Ignore unknown manifest entries. Note that this" << ::std::endl
       << "                          option also ignores the version constraints in the" << ::std::endl
       << "                          special toolchain build-time dependencies." << ::std::endl;

    os << std::endl
       << "\033[1m--min-bpkg-version\033[0m \033[4mver\033[0m    Apply backward compatibility workarounds to the" << ::std::endl
       << "                          generated \033[1mpackages.manifest\033[0m file so that it can be" << ::std::endl
       << "                          consumed by \033[1mbpkg\033[0m versions greater or equal to the" << ::std::endl
       << "                          specified version. If unspecified, then the" << ::std::endl
       << "                          \033[1mmin-bpkg-version\033[0m value from the" << ::std::endl
       << "                          \033[1mrepositories.manifest\033[0m file is used, if present. If" << ::std::endl
       << "                          the manifest value is not specified either, then no" << ::std::endl
       << "                          backward compatibility workarounds are applied." << ::std::endl;

    os << std::endl
       << "\033[1m--key\033[0m \033[4mname\033[0m                Private key to use to sign the repository. In most" << ::std::endl
       << "                          cases \033[4mname\033[0m will be a path to the key file but it can" << ::std::endl
       << "                          also be a key id when a custom \033[1mopenssl\033[0m cryptographic" << ::std::endl
       << "                          engine is used." << ::std::endl;

    p = ::bpkg::cli::usage_para::option;

    // common_options base
    //
    p = ::bpkg::common_options::print_usage (os, p);

    return p;
  }

  typedef
  std::map<std::string, void (*) (rep_create_options&, ::bpkg::cli::scanner&)>
  _cli_rep_create_options_map;

  static _cli_rep_create_options_map _cli_rep_create_options_map_;

  struct _cli_rep_create_options_map_init
  {
    _cli_rep_create_options_map_init ()
    {
      _cli_rep_create_options_map_["--ignore-unknown"] =
      &::bpkg::cli::thunk< rep_create_options, &rep_create_options::ignore_unknown_ >;
      _cli_rep_create_options_map_["--min-bpkg-version"] =
      &::bpkg::cli::thunk< rep_create_options, butl::standard_version, &rep_create_options::min_bpkg_version_,
        &rep_create_options::min_bpkg_version_specified_ >;
      _cli_rep_create_options_map_["--key"] =
      &::bpkg::cli::thunk< rep_create_options, string, &rep_create_options::key_,
        &rep_create_options::key_specified_ >;
    }
  };

  static _cli_rep_create_options_map_init _cli_rep_create_options_map_init_;

  bool rep_create_options::
  _parse (const char* o, ::bpkg::cli::scanner& s)
  {
    _cli_rep_create_options_map::const_iterator i (_cli_rep_create_options_map_.find (o));

    if (i != _cli_rep_create_options_map_.end ())
    {
      (*(i->second)) (*this, s);
      return true;
    }

    // common_options base
    //
    if (::bpkg::common_options::_parse (o, s))
      return true;

    return false;
  }

  bool rep_create_options::
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
  print_bpkg_rep_create_usage (::std::ostream& os, ::bpkg::cli::usage_para p)
  {
    CLI_POTENTIALLY_UNUSED (os);

    if (p != ::bpkg::cli::usage_para::none)
      os << ::std::endl;

    os << "\033[1mSYNOPSIS\033[0m" << ::std::endl
       << ::std::endl
       << "\033[1mbpkg rep-create\033[0m [\033[4moptions\033[0m] [\033[4mdir\033[0m]\033[0m" << ::std::endl
       << ::std::endl
       << "\033[1mDESCRIPTION\033[0m" << ::std::endl
       << ::std::endl
       << "The \033[1mrep-create\033[0m command regenerates the \033[1mpackages.manifest\033[0m file based on the" << ::std::endl
       << "files present in the repository directory. If the \033[1mrepositories.manifest\033[0m file" << ::std::endl
       << "contains a certificate, then the \033[1msignature.manifest\033[0m file is regenerated as" << ::std::endl
       << "well. In this case the \033[1m--key\033[0m option must be used to specify the certificate's" << ::std::endl
       << "private key. If \033[4mdir\033[0m is not specified, then the current working directory is" << ::std::endl
       << "used as the repository root." << ::std::endl;

    p = ::bpkg::rep_create_options::print_usage (os, ::bpkg::cli::usage_para::text);

    if (p != ::bpkg::cli::usage_para::none)
      os << ::std::endl;

    os << "\033[1mDEFAULT OPTIONS FILES\033[0m" << ::std::endl
       << ::std::endl
       << "See \033[1mbpkg-default-options-files(1)\033[0m for an overview of the default options files." << ::std::endl
       << "For the \033[1mrep-create\033[0m command the search start directory is the repository" << ::std::endl
       << "directory. The following options files are searched for in each directory and," << ::std::endl
       << "if found, loaded in the order listed:" << ::std::endl
       << ::std::endl
       << "bpkg.options" << ::std::endl
       << "bpkg-rep-create.options" << ::std::endl
       << ::std::endl
       << "The following \033[1mrep-create\033[0m command options cannot be specified in the remote" << ::std::endl
       << "default options files:" << ::std::endl
       << ::std::endl
       << "--key" << ::std::endl;

    p = ::bpkg::cli::usage_para::text;

    return p;
  }
}

// Begin epilogue.
//
//
// End epilogue.

