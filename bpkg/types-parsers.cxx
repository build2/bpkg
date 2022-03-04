// file      : bpkg/types-parsers.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/types-parsers.hxx>

namespace bpkg
{
  namespace cli
  {
    void parser<url>::
    parse (url& x, bool& xs, scanner& s)
    {
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const char* v (s.next ());

      try
      {
        x = url (v);
      }
      catch (const invalid_argument& e)
      {
        throw invalid_value (o, v, e.what ());
      }

      xs = true;
    }

    template <typename T>
    static void
    parse_path (T& x, scanner& s)
    {
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const char* v (s.next ());

      try
      {
        x = T (v);

        if (x.empty ())
          throw invalid_value (o, v);
      }
      catch (const invalid_path&)
      {
        throw invalid_value (o, v);
      }
    }

    void parser<path>::
    parse (path& x, bool& xs, scanner& s)
    {
      xs = true;
      parse_path (x, s);
    }

    void parser<dir_path>::
    parse (dir_path& x, bool& xs, scanner& s)
    {
      xs = true;
      parse_path (x, s);
    }

    void parser<uuid>::
    parse (uuid& x, bool& xs, scanner& s)
    {
      xs = true;

      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const char* v (s.next ());

      try
      {
        x = uuid (v);

        if (x.nil ())
          throw invalid_value (o, v);
      }
      catch (const invalid_argument&)
      {
        throw invalid_value (o, v);
      }
    }

    void parser<butl::standard_version>::
    parse (butl::standard_version& x, bool& xs, scanner& s)
    {
      using butl::standard_version;

      xs = true;

      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const char* v (s.next ());

      try
      {
        // Note that we allow all kinds of versions, so that the caller can
        // restrict them as they wish after the parsing.
        //
        x = standard_version (v,
                              standard_version::allow_earliest |
                              standard_version::allow_stub);
      }
      catch (const invalid_argument& e)
      {
        throw invalid_value (o, v, e.what ());
      }
    }

    void parser<auth>::
    parse (auth& x, bool& xs, scanner& s)
    {
      xs = true;
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const string v (s.next ());
      if (v == "none")
        x = auth::none;
      else if (v == "remote")
        x = auth::remote;
      else if (v == "all")
        x = auth::all;
      else
        throw invalid_value (o, v);
    }

    void parser<stdout_format>::
    parse (stdout_format& x, bool& xs, scanner& s)
    {
      xs = true;
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const string v (s.next ());
      if (v == "lines")
        x = stdout_format::lines;
      else if (v == "json")
        x = stdout_format::json;
      else
        throw invalid_value (o, v);
    }

    void parser<repository_type>::
    parse (repository_type& x, bool& xs, scanner& s)
    {
      xs = true;
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const string v (s.next ());

      try
      {
        x = to_repository_type (v);
      }
      catch (const invalid_argument&)
      {
        throw invalid_value (o, v);
      }
    }
  }
}
