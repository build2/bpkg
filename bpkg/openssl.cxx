// file      : bpkg/openssl.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/openssl.hxx>

#include <butl/process>

#include <bpkg/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  process
  start_openssl (const common_options& co,
                 const char* command,
                 const cstrings& options,
                 bool in,
                 bool out,
                 bool err)
  {
    cstrings args {co.openssl ().string ().c_str (), command};

    // Add extra options. Normally the order of options is not important
    // (unless they override each other). However, openssl 1.0.1 seems to have
    // bugs in that department (that were apparently fixed in 1.0.2). To work
    // around these bugs we pass user-supplied options first.
    //
    for (const string& o: co.openssl_option ())
      args.push_back (o.c_str ());

    args.insert (args.end (), options.begin (), options.end ());
    args.push_back (nullptr);

    try
    {
      process_path pp (process::path_search (args[0]));

      if (verb >= 2)
        print_process (args);

      // If the caller is interested in reading STDOUT and STDERR, then
      // redirect STDERR to STDOUT, so both can be read from the same stream.
      //
      return process (
        pp, args.data (), in ? -1 : 0, out ? -1 : 1, err ? (out ? 1 : -1): 2);
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e;

      if (e.child)
        exit (1);

      throw failed ();
    }
  }
}
