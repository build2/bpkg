// file      : bpkg/types-parsers.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/types-parsers.hxx>

#include <libbpkg/manifest.hxx>

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

    void parser<git_protocol_capabilities>::
    parse (git_protocol_capabilities& x, bool& xs, scanner& s)
    {
      xs = true;
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      const string v (s.next ());
      if (v == "dumb")
        x = git_protocol_capabilities::dumb;
      else if (v == "smart")
        x = git_protocol_capabilities::smart;
      else if (v == "unadv")
        x = git_protocol_capabilities::unadv;
      else
        throw invalid_value (o, v);
    }

    void parser<git_capabilities_map>::
    parse (git_capabilities_map& x, bool& xs, scanner& s)
    {
      xs = true;
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      string v (s.next ());
      size_t p (v.rfind ('='));

      if (p == string::npos)
        throw invalid_value (o, v);

      string k (v, 0, p);

      // Verify that the key is a valid remote git repository URL prefix.
      //
      try
      {
        repository_url u (k);

        if (u.scheme == repository_protocol::file)
          throw invalid_value (o, k, "local repository location");
      }
      catch (const invalid_argument& e)
      {
        throw invalid_value (o, k, e.what ());
      }

      // Parse the protocol capabilities value.
      //
      int ac (2);
      char* av[] = {const_cast<char*> (o),
                    const_cast<char*> (v.c_str () + p + 1)};

      argv_scanner vs (0, ac, av);

      bool dummy;
      parser<git_protocol_capabilities>::parse (x[k], dummy, vs);
    }

    void parser<git_capabilities_map>::
    merge (git_capabilities_map& b, const git_capabilities_map& a)
    {
      for (const auto& o: a)
      {
        auto i (b.find (o.first));

        if (i != b.end ())
          i->second = o.second;
        else
          b.emplace (o.first, o.second);
      }
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
