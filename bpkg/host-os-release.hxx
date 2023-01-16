// file      : bpkg/host-os-release.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_HOST_OS_RELEASE_HXX
#define BPKG_HOST_OS_RELEASE_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

namespace bpkg
{
  // Information extracted from /etc/os-release on Linux. See os-release(5)
  // for background. For other platforms we derive the equivalent information
  // from other sources. Some examples:
  //
  // {"debian", {}, "10", "",
  //  "Debian GNU/Linux", "buster", ""}
  //
  // {"fedora", {}, "35", "workstation",
  //  "Fedora Linux", "", "Workstation Edition"}
  //
  // {"ubuntu", {"debian"}, "20.04", "",
  //  "Ubuntu", "focal", ""}
  //
  // {"windows", {}, "10", "",
  //  "Windows", "", ""}
  //
  // Note that version_id may be empty, for example, on Debian testing:
  //
  // {"debian", {}, "", "",
  //  "Debian GNU/Linux", "", ""}
  //
  // Note also that we don't extract PRETTY_NAME because its content is
  // unpredictable. For example, it may include variant, as in "Fedora Linux
  // 35 (Workstation Edition)". Instead, construct it from the individual
  // components as appropriate, normally "$name $version ($version_codename)".
  //
  struct os_release
  {
    string         name_id;    // ID
    vector<string> like_ids;   // ID_LIKE
    string         version_id; // VERSION_ID
    string         variant_id; // VARIANT_ID

    string name;             // NAME
    string version_codename; // VERSION_CODENAME
    string variant;          // VARIANT
  };

  // Return the release information for the specified host or nullopt if
  // the specific host is unknown/unsupported.
  //
  optional<os_release>
  host_os_release (const target_triplet& host);
}

#endif // BPKG_HOST_OS_RELEASE_HXX
