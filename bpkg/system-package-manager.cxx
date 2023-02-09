// file      : bpkg/system-package-manager.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager.hxx>

#include <sstream>

#include <libbutl/regex.hxx>
#include <libbutl/semantic-version.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

#include <bpkg/pkg-bindist-options.hxx>

#include <bpkg/system-package-manager-debian.hxx>
#include <bpkg/system-package-manager-fedora.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  system_package_manager::
  ~system_package_manager ()
  {
    // vtable
  }

  // Return true if the specified operating system is or like the specified
  // id.
  //
  static inline bool
  is_or_like (const os_release& os, const char* id)
  {
    return (os.name_id == id ||
            find_if (os.like_ids.begin (), os.like_ids.end (),
                     [id] (const string& n)
                     {
                       return n == id;
                     }) != os.like_ids.end ());
  }

  unique_ptr<system_package_manager>
  make_consumption_system_package_manager (const common_options& co,
                                           const target_triplet& host,
                                           const string& name,
                                           const string& arch,
                                           bool install,
                                           bool fetch,
                                           bool yes,
                                           const string& sudo)
  {
    // Note: similar to make_consumption_system_package_manager() below.

    optional<bool> progress (co.progress ()    ? true  :
                             co.no_progress () ? false :
                             optional<bool> ());

    optional<size_t> fetch_timeout (co.fetch_timeout_specified ()
                                    ? co.fetch_timeout ()
                                    : optional<size_t> ());

    unique_ptr<system_package_manager> r;

    if (optional<os_release> oos = host_os_release (host))
    {
      os_release& os (*oos);

      if (host.class_ == "linux")
      {
        if (is_or_like (os, "debian") ||
            is_or_like (os, "ubuntu"))
        {
          if (!name.empty () && name != "debian")
            fail << "unsupported package manager '" << name << "' for "
                 << os.name_id << " host";

          // If we recognized this as Debian-like in an ad hoc manner, then
          // add debian to like_ids.
          //
          if (os.name_id != "debian" && !is_or_like (os, "debian"))
            os.like_ids.push_back ("debian");

          r.reset (new system_package_manager_debian (
                     move (os), host, arch,
                     progress, fetch_timeout, install, fetch, yes, sudo));
        }
        else if (is_or_like (os, "fedora") ||
                 is_or_like (os, "rhel")   ||
                 is_or_like (os, "centos") ||
                 is_or_like (os, "rocky")  ||
                 is_or_like (os, "almalinux"))
        {
          if (!name.empty () && name != "fedora")
            fail << "unsupported package manager '" << name << "' for "
                 << os.name_id << " host";

          // If we recognized this as Fedora-like in an ad hoc manner, then
          // add fedora to like_ids.
          //
          if (os.name_id != "fedora" && !is_or_like (os, "fedora"))
            os.like_ids.push_back ("fedora");

          r.reset (new system_package_manager_fedora (
                     move (os), host, arch,
                     progress, fetch_timeout, install, fetch, yes, sudo));
        }
        // NOTE: remember to update the --sys-distribution pkg-build option
        //       documentation if adding support for another package manager.
      }
    }

    if (r == nullptr)
    {
      if (!name.empty ())
        fail << "unsupported package manager '" << name << "' for host "
             << host;
    }

    return r;
  }

  unique_ptr<system_package_manager>
  make_production_system_package_manager (const pkg_bindist_options& o,
                                          const target_triplet& host,
                                          const string& name,
                                          const string& arch)
  {
    // Note: similar to make_production_system_package_manager() above.

    optional<bool> progress (o.progress () ? true :
                             o.no_progress () ? false :
                             optional<bool> ());

    unique_ptr<system_package_manager> r;

    if (optional<os_release> oos = host_os_release (host))
    {
      os_release& os (*oos);

      if (host.class_ == "linux")
      {
        if (is_or_like (os, "debian") ||
            is_or_like (os, "ubuntu"))
        {
          if (!name.empty () && name != "debian")
            fail << "unsupported package manager '" << name << "' for "
                 << os.name_id << " host";

          if (os.name_id != "debian" && !is_or_like (os, "debian"))
            os.like_ids.push_back ("debian");

          r.reset (new system_package_manager_debian (
                     move (os), host, arch, progress));
        }
        else if (is_or_like (os, "fedora") ||
                 is_or_like (os, "rhel")   ||
                 is_or_like (os, "centos") ||
                 is_or_like (os, "rocky")  ||
                 is_or_like (os, "almalinux"))
        {
          if (!name.empty () && name != "fedora")
            fail << "unsupported package manager '" << name << "' for "
                 << os.name_id << " host";

          if (os.name_id != "fedora" && !is_or_like (os, "fedora"))
            os.like_ids.push_back ("fedora");

          r.reset (new system_package_manager_fedora (
                     move (os), host, arch, progress));
        }
        // NOTE: remember to update the --distribution pkg-bindist option
        //       documentation if adding support for another package manager.
      }
    }

    if (r == nullptr)
    {
      if (!name.empty ())
        fail << "unsupported package manager '" << name << "' for host "
             << host;
    }

    return r;
  }

  // Return the version id parsed as a semantic version if it is not empty and
  // the "0" semantic version otherwise. Issue diagnostics and fail on parsing
  // errors.
  //
  // Note: the name_id argument is only used for diagnostics.
  //
  static inline semantic_version
  parse_version_id (const string& version_id, const string& name_id)
  {
    if (version_id.empty ())
      return semantic_version (0, 0, 0);

    try
    {
      return semantic_version (version_id, semantic_version::allow_omit_minor);
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid version '" << version_id << "' for " << name_id
           << " host: " << e << endf;
    }
  }

  // Parse the <distribution> component of the specified <distribution>-*
  // value into the distribution name and version (return as "0" if not
  // present). Issue diagnostics and fail on parsing errors.
  //
  // Note: the value_name, ap, and af arguments are only used for diagnostics.
  //
  static pair<string, semantic_version>
  parse_distribution (string&& d,
                      const string& value_name,
                      const available_package& ap,
                      const lazy_shared_ptr<repository_fragment>& af)
  {
    string dn (move (d));      // <name>[_<version>]
    size_t p (dn.rfind ('_')); // Version-separating underscore.

    // If the '_' separator is present, then make sure that the right-hand
    // part looks like a version (not empty and only contains digits and
    // dots).
    //
    if (p != string::npos)
    {
      if (p != dn.size () - 1)
      {
        for (size_t i (p + 1); i != dn.size (); ++i)
        {
          if (!digit (dn[i]) && dn[i] != '.')
          {
            p = string::npos;
            break;
          }
        }
      }
      else
        p = string::npos;
    }

    // Parse the distribution version if present and leave it "0" otherwise.
    //
    semantic_version dv (0, 0, 0);
    if (p != string::npos)
    try
    {
      dv = semantic_version (dn,
                             p + 1,
                             semantic_version::allow_omit_minor);

      dn.resize (p);
    }
    catch (const invalid_argument& e)
    {
      // Note: the repository fragment may have no database associated when
      // used in tests.
      //
      shared_ptr<repository_fragment> f (af.get_eager ());
      database* db (!(f != nullptr && !af.loaded ()) // Not transient?
                    ? &af.database ()
                    : nullptr);

      diag_record dr (fail);
      dr << "invalid distribution version '" << string (dn, p + 1)
         << "' in value " << value_name << " for package " << ap.id.name
         << ' ' << ap.version;

      if (db != nullptr)
        dr << *db;

      dr << " in repository " << (f != nullptr ? f : af.load ())->location
         << ": " << e;
    }

    return make_pair (move (dn), move (dv));
  }

  strings system_package_manager::
  system_package_names (const available_packages& aps,
                        const string& name_id,
                        const string& version_id,
                        const vector<string>& like_ids)
  {
    assert (!aps.empty ());

    semantic_version vid (parse_version_id (version_id, name_id));

    // Return those <name>[_<version>]-name distribution values of the
    // specified available packages whose <name> component matches the
    // specified distribution name and the <version> component (assumed as "0"
    // if not present) is less or equal the specified distribution version.
    // Suppress duplicate values.
    //
    auto name_values = [&aps] (const string& n, const semantic_version& v)
    {
      strings r;

      // For each available package sort the system package names in the
      // distribution version descending order and then append them to the
      // resulting list, keeping this order and suppressing duplicates.
      //
      using name_version = pair<string, semantic_version>;
      vector<name_version> nvs; // Reuse the buffer.

      for (const auto& a: aps)
      {
        nvs.clear ();

        const shared_ptr<available_package>& ap (a.first);

        for (const distribution_name_value& dv: ap->distribution_values)
        {
          if (optional<string> d = dv.distribution ("-name"))
          {
            pair<string, semantic_version> dnv (
              parse_distribution (move (*d), dv.name, *ap, a.second));

            semantic_version& dvr (dnv.second);

            if (dnv.first == n && dvr <= v)
            {
              // Add the name/version pair to the sorted vector.
              //
              name_version nv (make_pair (dv.value, move (dvr)));

              nvs.insert (upper_bound (nvs.begin (), nvs.end (), nv,
                                       [] (const name_version& x,
                                           const name_version& y)
                                       {return x.second > y.second;}),
                          move (nv));
            }
          }
        }

        // Append the sorted names to the resulting list.
        //
        for (name_version& nv: nvs)
        {
          if (find_if (r.begin (), r.end (),
                       [&nv] (const string& n) {return nv.first == n;}) ==
              r.end ())
          {
            r.push_back (move (nv.first));
          }
        }
      }

      return r;
    };

    // Collect distribution values for those <distribution>-name names which
    // match the name id and refer to the version which is less or equal than
    // the version id.
    //
    strings r (name_values (name_id, vid));

    // If the resulting list is empty and the like ids are specified, then
    // re-collect but now using the like id and "0" version id instead.
    //
    if (r.empty ())
    {
      for (const string& like_id: like_ids)
      {
        r = name_values (like_id, semantic_version (0, 0, 0));
        if (!r.empty ())
          break;
      }
    }

    return r;
  }

  optional<string> system_package_manager::
  system_package_version (const available_package& ap,
                          const lazy_shared_ptr<repository_fragment>& af,
                          const string& name_id,
                          const string& version_id,
                          const vector<string>& like_ids)
  {
    semantic_version vid (parse_version_id (version_id, name_id));

    // Iterate over the <name>[_<version>]-version distribution values of the
    // passed available package. Only consider those values whose <name>
    // component matches the specified distribution name and the <version>
    // component (assumed as "0" if not present) is less or equal the
    // specified distribution version. Return the system package version if
    // the distribution version is equal to the specified one. Otherwise (the
    // version is less), continue iterating while preferring system version
    // candidates for greater distribution versions. Note that here we are
    // trying to pick the system version which distribution version closest
    // (but never greater) to the specified distribution version, similar to
    // what we do in downstream_package_version() (see its
    // downstream_version() lambda for details).
    //
    auto system_version = [&ap, &af] (const string& n,
                                      const semantic_version& v)
      -> optional<string>
    {
      optional<string> r;
      semantic_version rv;

      for (const distribution_name_value& dv: ap.distribution_values)
      {
        if (optional<string> d = dv.distribution ("-version"))
        {
          pair<string, semantic_version> dnv (
            parse_distribution (move (*d), dv.name, ap, af));

          semantic_version& dvr (dnv.second);

          if (dnv.first == n && dvr <= v)
          {
            // If the distribution version is equal to the specified one, then
            // we are done. Otherwise, save the system version if it is
            // preferable and continue iterating.
            //
            if (dvr == v)
              return move (dv.value);

            if (!r || rv < dvr)
            {
              r = move (dv.value);
              rv = move (dvr);
            }
          }
        }
      }

      return r;
    };

    // Try to deduce the system package version using the
    // <distribution>-version values that match the name id and refer to the
    // version which is less or equal than the version id.
    //
    optional<string> r (system_version (name_id, vid));

    // If the system package version is not deduced and the like ids are
    // specified, then re-try but now using the like id and "0" version id
    // instead.
    //
    if (!r)
    {
      for (const string& like_id: like_ids)
      {
        r = system_version (like_id, semantic_version (0, 0, 0));
        if (r)
          break;
      }
    }

    return r;

  }

  optional<version> system_package_manager::
  downstream_package_version (const string& system_version,
                              const available_packages& aps,
                              const string& name_id,
                              const string& version_id,
                              const vector<string>& like_ids)
  {
    assert (!aps.empty ());

    semantic_version vid (parse_version_id (version_id, name_id));

    // Iterate over the passed available packages (in version descending
    // order) and over the <name>[_<version>]-to-downstream-version
    // distribution values they contain. Only consider those values whose
    // <name> component matches the specified distribution name and the
    // <version> component (assumed as "0" if not present) is less or equal
    // the specified distribution version. For such values match the regex
    // pattern against the passed system version and if it matches consider
    // the replacement as the resulting downstream version candidate. Return
    // this downstream version if the distribution version is equal to the
    // specified one. Otherwise (the version is less), continue iterating
    // while preferring downstream version candidates for greater distribution
    // versions. Note that here we are trying to use a version mapping for the
    // distribution version closest (but never greater) to the specified
    // distribution version. So, for example, if both following values contain
    // a matching mapping, then for debian 11 we prefer the downstream version
    // produced by the debian_10-to-downstream-version value:
    //
    // debian_9-to-downstream-version
    // debian_10-to-downstream-version
    //
    auto downstream_version = [&aps, &system_version]
                              (const string& n,
                               const semantic_version& v) -> optional<version>
    {
      optional<version> r;
      semantic_version rv;

      for (const auto& a: aps)
      {
        const shared_ptr<available_package>& ap (a.first);

        for (const distribution_name_value& nv: ap->distribution_values)
        {
          if (optional<string> d = nv.distribution ("-to-downstream-version"))
          {
            pair<string, semantic_version> dnv (
              parse_distribution (move (*d), nv.name, *ap, a.second));

            semantic_version& dvr (dnv.second);

            if (dnv.first == n && dvr <= v)
            {
              auto bad_value = [&nv, &ap, &a] (const string& d)
              {
                // Note: the repository fragment may have no database
                // associated when used in tests.
                //
                const lazy_shared_ptr<repository_fragment>& af (a.second);
                shared_ptr<repository_fragment> f (af.get_eager ());
                database* db (!(f != nullptr && !af.loaded ()) // Not transient?
                              ? &af.database ()
                              : nullptr);

                diag_record dr (fail);
                dr << "invalid distribution value '" << nv.name << ": "
                   << nv.value << "' for package " << ap->id.name << ' '
                   << ap->version;

                if (db != nullptr)
                  dr << *db;

                dr << " in repository "
                   << (f != nullptr ? f : af.load ())->location << ": " << d;
              };

              // Parse the distribution value into the regex pattern and the
              // replacement.
              //
              // Note that in the future we may add support for some regex
              // flags.
              //
              pair<string, string> rep;
              try
              {
                size_t end;
                const string& val (nv.value);
                rep = regex_replace_parse (val.c_str (), val.size (), end);
              }
              catch (const invalid_argument& e)
              {
                bad_value (e.what ());
              }

              // Match the regex pattern against the system version and skip
              // the value if it doesn't match or proceed to parsing the
              // downstream version resulting from the regex replacement
              // otherwise.
              //
              string dv;
              try
              {
                regex re (rep.first, regex::ECMAScript);

                pair<string, bool> rr (
                  regex_replace_match (system_version, re, rep.second));

                // Skip the regex if it doesn't match.
                //
                if (!rr.second)
                  continue;

                dv = move (rr.first);
              }
              catch (const regex_error& e)
              {
                // Print regex_error description if meaningful (no space).
                //
                ostringstream os;
                os << "invalid regex pattern '" << rep.first << "'" << e;
                bad_value (os.str ());
              }

              // Parse the downstream version.
              //
              try
              {
                version ver (dv);

                // If the distribution version is equal to the specified one,
                // then we are done. Otherwise, save the downstream version if
                // it is preferable and continue iterating.
                //
                // Note that bailing out immediately in the former case is
                // essential. Otherwise, we can potentially fail later on, for
                // example, some ill-formed regex which is already fixed in
                // some newer package.
                //
                if (dvr == v)
                  return ver;

                if (!r || rv < dvr)
                {
                  r = move (ver);
                  rv = move (dvr);
                }
              }
              catch (const invalid_argument& e)
              {
                bad_value ("resulting downstream version '" + dv +
                           "' is invalid: " + e.what ());
              }
            }
          }
        }
      }

      return r;
    };

    // Try to deduce the downstream version using the
    // <distribution>-to-downstream-version values that match the name id and
    // refer to the version which is less or equal than the version id.
    //
    optional<version> r (downstream_version (name_id, vid));

    // If the downstream version is not deduced and the like ids are
    // specified, then re-try but now using the like id and "0" version id
    // instead.
    //
    if (!r)
    {
      for (const string& like_id: like_ids)
      {
        r = downstream_version (like_id, semantic_version (0, 0, 0));
        if (r)
          break;
      }
    }

    return r;
  }
}
