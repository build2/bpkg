// file      : bpkg/types-parsers.txx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace bpkg
{
  namespace cli
  {
    template <const char* const* Q, typename V>
    void parser<qualified_option<Q, V>>::
    parse (qualified_option<Q, V>& x, bool& xs, scanner& s)
    {
      xs = true;
      const char* o (s.next ());

      if (!s.more ())
        throw missing_value (o);

      string v (s.next ());

      // Extract the qualifier from the option value.
      //
      string qv;
      size_t n (v.find (':'));

      if (n != string::npos)
      {
        const char* const* q (Q);
        for (; *q != nullptr; ++q)
        {
          if (v.compare (0, n, *q) == 0)
          {
            qv = *q;
            v = string (v, n + 1);
            break;
          }
        }

        // Fail it the qualifier is not recognized, unless it is a special
        // empty qualifier.
        //
        if (*q == nullptr && n != 0)
          throw invalid_value (o, v);
      }

      // Parse the value for the extracted (possibly empty) qualifier.
      //
      int ac (2);
      char* av[] = {const_cast<char*> (o), const_cast<char*> (v.c_str ())};
      bool dummy;

      {
        argv_scanner s (0, ac, av);
        parser<V>::parse (x[qv], dummy, s);
      }

      // Parse an unqualified value for all qualifiers.
      //
      if (qv.empty ())
      {
        for (const char* const* q (Q); *q != nullptr; ++q)
        {
          argv_scanner s (0, ac, av);
          parser<V>::parse (x[*q], dummy, s);
        }
      }
    }

    template <const char* const* Q, typename V>
    void parser<qualified_option<Q, V>>::
    merge (qualified_option<Q, V>& b, const qualified_option<Q, V>& a)
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
  }
}
