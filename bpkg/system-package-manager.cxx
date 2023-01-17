// file      : bpkg/system-package-manager.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/system-package-manager.hxx>

#include <libbutl/semantic-version.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  system_package_manager::
  ~system_package_manager ()
  {
    // vtable
  }

  unique_ptr<system_package_manager>
  make_system_package_manager (const target_triplet& host,
                               const string& name)
  {
    unique_ptr<system_package_manager> r;

    if (optional<os_release> osr = host_os_release (host))
    {
      if (host.class_ == "linux")
      {
        if (osr->name_id == "debian" ||
            osr->name_id == "ubuntu" ||
            find_if (osr->like_ids.begin (), osr->like_ids.end (),
                     [] (const string& n)
                     {
                       return n == "debian" || n == "ubuntu";
                     }) != osr->like_ids.end ())
        {
          // @@ TODO: verify name if specified.

          //r.reset (new system_package_manager_debian (move (*osr)));
        }
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

  strings system_package_manager::
  system_package_names (const available_packages& aps,
                        const string& name_id,
                        const string& version_id,
                        const string& like_id)
  {
    assert (!aps.empty ());

    // Parse the version id if it is not empty and leave it "0" otherwise.
    //
    semantic_version vid;

    if (!version_id.empty ())
    try
    {
      vid = semantic_version (version_id, semantic_version::allow_omit_minor);
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid version '" << version_id << "' for " << name_id
           << " operating system: " << e;
    }

    // Return those <name>[_<version>]-name distribution values of the
    // specified available packages whose <name> component matches the
    // specified distribution name and the <version> component (assumed as "0"
    // if not present) is less or equal the specified distribution version.
    // Suppress duplicate entries with the same name (so that distribution
    // values of the later available package versions are preferred) or value.
    //
    using values = vector<reference_wrapper<const distribution_name_value>>;

    auto name_values = [&aps] (const string& n, const semantic_version& v)
    {
      values r;
      for (const auto& a: aps)
      {
        const shared_ptr<available_package>& ap (a.first);

        for (const distribution_name_value& nv: ap->distribution_values)
        {
          const string& nm (nv.name);

          // @@ TODO
          //
          // optional<string>
          // distribution_name_value::distribution (const string& suffix);
          //
          if (nm.size () > 5 && nm.compare (nm.size () - 5, 5, "-name") == 0)
          {
            string dn (nm, 0, nm.size () - 5); // <name>[_<version>]
            size_t p (dn.rfind ('_'));         // Version-separating underscore.

            // If '_' separator is present, then make sure that the right-hand
            // part looks like a version (not empty and only contains digits
            // and dots).
            //
            if (p != string::npos)
            {
              if (p != dn.size () - 1)
              {
                for (size_t i (p); i != dn.size (); ++i)
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

            // Parse the distribution version if present and leave it "0"
            // otherwise.
            //
            semantic_version dv;
            if (p != string::npos)
            try
            {
              dv = semantic_version (dn,
                                     p + 1,
                                     semantic_version::allow_omit_minor);
            }
            catch (const invalid_argument& e)
            {
              // @@ []
              //
              const string& db (a.second.database ().string);

              fail << "invalid distribution version in value " << nm
                   << " for package " << ap->id.name << ' ' << ap->version
                   << (!db.empty () ? ' ' + db : empty_string)
                   << " in repository " << a.second.load ()->location << ": "
                   << e;
            }

            dn.resize (p);

            if (dn == n &&
                dv <= v &&
                find_if (r.begin (), r.end (),
                         [&nm, &nv] (const distribution_name_value& v)
                         {return v.name == nm || v.value == nv.value;}) ==
                r.end ())
            {
              r.push_back (nv);
            }
          }
        }
      }

      return r;
    };

    // Collect the <distribution>-name values that match the name id and refer
    // to the version which is less or equal than the version id.
    //
    values vs (name_values (name_id, vid));

    // If the resulting list is empty and the like id is specified, then
    // re-collect but now using the like id and "0" version id instead.
    //
    if (vs.empty () && !like_id.empty ())
      vs = name_values (like_id, semantic_version (0, 0, 0));

    // Return the values of the collected name/values list.
    //
    strings r;
    r.reserve (vs.size ());

    transform (vs.begin (), vs.end (),
               back_inserter (r),
               [] (const distribution_name_value& v) {return v.value;});

    return r;
  }
}
