// file      : bpkg/pkg-test.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_PKG_TEST_HXX
#define BPKG_PKG_TEST_HXX

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/pkg-command.hxx>
#include <bpkg/pkg-test-options.hxx>

namespace bpkg
{
  inline int
  pkg_test (const pkg_test_options& o, cli::group_scanner& args)
  {
    return pkg_command ("test",
                        o,
                        "" /* cmd_variant */,
                        o.recursive (),
                        o.immediate (),
                        o.all (),
                        o.all_pattern (),
                        o.package_cwd (),
                        true /* allow_host_type */,
                        args);
  }
}

#endif // BPKG_PKG_TEST_HXX
