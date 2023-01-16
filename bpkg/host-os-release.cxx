// file      : bpkg/host-os-release.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/host-os-release.hxx>

#include <libbutl/string-parser.hxx> // parse_quoted()

#include <bpkg/diagnostics.hxx>

using namespace butl;

namespace bpkg
{
  // Note: not static for access from the unit test.
  //
  os_release
  host_os_release_linux (path f = {})
  {
    os_release r;

    // According to os-release(5), we should use /etc/os-release and fallback
    // to /usr/lib/os-release if the former does not exist. It also lists the
    // fallback values for individual variables, in case some are not present.
    //
    if (!f.empty ()
        ? exists (f)
        : (exists (f = path ("/etc/os-release")) ||
           exists (f = path ("/usr/lib/os-release"))))
    {
      try
      {
        ifdstream ifs (f, ifdstream::badbit);

        string l;
        for (uint64_t ln (1); !eof (getline (ifs, l)); ++ln)
        {
          trim (l);

          // Skip blanks lines and comments.
          //
          if (l.empty () || l[0] == '#')
            continue;

          // The variable assignments are in the "shell style" and so can be
          // quoted/escaped. For now we only handle quoting, which is what all
          // the instances seen in the wild seems to use.
          //
          size_t p (l.find ('='));
          if (p == string::npos)
            continue;

          string n (l, 0, p);
          l.erase (0, p + 1);

          using string_parser::parse_quoted;
          using string_parser::invalid_string;

          try
          {
            if (n == "ID_LIKE")
            {
              r.like_ids.clear ();

              vector<string> vs (parse_quoted (l, true /* unquote */));
              for (const string& v: vs)
              {
                for (size_t b (0), e (0); next_word (v, b, e); )
                {
                  r.like_ids.push_back (string (v, b, e - b));
                }
              }
            }
            else if (string* p = (n == "ID"               ?  &r.name_id :
                                  n == "VERSION_ID"       ?  &r.version_id :
                                  n == "VARIANT_ID"       ?  &r.variant_id :
                                  n == "NAME"             ?  &r.name :
                                  n == "VERSION_CODENAME" ?  &r.version_codename :
                                  n == "VARIANT"          ?  &r.variant :
                                  nullptr))
            {
              vector<string> vs (parse_quoted (l, true /* unquote */));
              switch (vs.size ())
              {
              case 0:  *p =  ""; break;
              case 1:  *p = move (vs.front ()); break;
              default: throw invalid_string (0, "multiple values");
              }
            }
          }
          catch (const invalid_string& e)
          {
            location loc (move (f).string (), ln);
            fail (loc) << "invalid " << n << " value: " << e;
          }
        }

        ifs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to read from " << f << ": " << e;
      }
    }

    // Assign fallback values.
    //
    if (r.name_id.empty ()) r.name_id = "linux";
    if (r.name.empty ())    r.name    = "Linux";

    return r;
  }

  optional<os_release>
  host_os_release (const target_triplet& host)
  {
    if (host.class_ == "linux")
      return host_os_release_linux ();
    else
      return nullopt;
  }
}
