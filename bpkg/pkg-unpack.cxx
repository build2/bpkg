// file      : bpkg/pkg-unpack.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-unpack>

#include <butl/process>

#include <bpkg/manifest>

#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/database>
#include <bpkg/diagnostics>

#include <bpkg/pkg-purge>
#include <bpkg/pkg-verify>

using namespace std;
using namespace butl;

namespace bpkg
{
  shared_ptr<selected_package>
  pkg_unpack (const dir_path& c,
              transaction& t,
              const dir_path& d,
              bool replace,
              bool purge)
  {
    tracer trace ("pkg_unpack");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    if (!exists (d))
      fail << "package directory " << d << " does not exist";

    // Verify the directory is a package and get its manifest.
    //
    package_manifest m (pkg_verify (d, true));
    l4 ([&]{trace << d << ": " << m.name << " " << m.version;});

    // Make the package and configuration paths absolute and normalized.
    // If the package is inside the configuration, use the relative path.
    // This way we can move the configuration around.
    //
    dir_path ac (c), ad (d);
    ac.complete ().normalize ();
    ad.complete ().normalize ();

    if (ad.sub (ac))
      ad = ad.leaf (ac);

    // See if this package already exists in this configuration.
    //
    const string& n (m.name);
    shared_ptr<selected_package> p (db.find<selected_package> (n));

    if (p != nullptr)
    {
      bool s (p->state == package_state::fetched ||
              p->state == package_state::unpacked);

      if (!replace || !s)
      {
        diag_record dr (fail);

        dr << "package " << n << " already exists in configuration " << c <<
          info << "version: " << p->version << ", state: " << p->state;

        if (s) // Suitable state for replace?
          dr << info << "use 'pkg-unpack --replace|-r' to replace";
      }

      // Clean up the source directory and archive of the package we are
      // replacing. Once this is done, there is no going back. If things
      // go badly, we can't simply abort the transaction.
      //
      pkg_purge_fs (c, t, p);

      // Use the special root repository as the repository of this package.
      //
      p->version = move (m.version);
      p->state = package_state::unpacked;
      p->repository = repository_location ();
      p->src_root = move (ad);
      p->purge_src = purge;

      db.update (p);
    }
    else
    {
      p.reset (new selected_package {
        move (m.name),
        move (m.version),
        package_state::unpacked,
        false,   // hold package
        false,   // hold version
        repository_location (), // Root repository.
        nullopt,    // No archive
        false,      // Don't purge archive.
        move (ad),
        purge,
        nullopt,    // No output directory yet.
        {}});       // No prerequisites captured yet.

      db.persist (p);
    }

    t.commit ();
    return p;
  }

  shared_ptr<selected_package>
  pkg_unpack (const common_options& co,
              const dir_path& c,
              transaction& t,
              const string& name)
  {
    tracer trace ("pkg_unpack");

    database& db (t.database ());
    tracer_guard tg (db, trace);

    shared_ptr<selected_package> p (db.find<selected_package> (name));

    if (p == nullptr)
      fail << "package " << name << " does not exist in configuration " << c;

    if (p->state != package_state::fetched)
      fail << "package " << name << " is " << p->state <<
        info << "expected it to be fetched";

    l4 ([&]{trace << p->name << " " << p->version;});

    assert (p->archive); // Should have archive in the fetched state.

    // If the archive path is not absolute, then it must be relative
    // to the configuration.
    //
    path a (p->archive->absolute () ? *p->archive : c / *p->archive);

    l4 ([&]{trace << "archive: " << a;});

    // Extract the package directory. Currently we always extract it
    // into the configuration directory. But once we support package
    // cache, this will need to change.
    //
    // Also, since we must have verified the archive during fetch,
    // here we can just assume what the resulting directory will be.
    //
    dir_path d (c / dir_path (p->name + '-' + p->version.string ()));

    if (exists (d))
      fail << "package directory " << d << " already exists";

    // What should we do if tar or something after it fails? Cleaning
    // up the package directory sounds like the right thing to do.
    //
    auto_rm_r arm (d);

    cstrings args {co.tar ().string ().c_str ()};

    // Add extra options.
    //
    for (const string& o: co.tar_option ())
      args.push_back (o.c_str ());

    // -C/--directory -- change to directory.
    //
    args.push_back ("-C");
    args.push_back (c.string ().c_str ());

    // An archive name that has a colon in it specifies a file or device on a
    // remote machine. That makes it impossible to use absolute Windows paths
    // unless we add the --force-local option.
    //
    args.push_back ("--force-local");

    args.push_back ("-xf");
    args.push_back (a.string ().c_str ());
    args.push_back (nullptr);

    if (verb >= 2)
      print_process (args);

    try
    {
      process pr (args.data ());

      // While it is reasonable to assuming the child process issued
      // diagnostics, tar, specifically, doesn't mention the archive
      // name.
      //
      if (!pr.wait ())
        fail << "unable to extract package archive " << a;
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << args[0] << ": " << e.what ();

      if (e.child ())
        exit (1);

      throw failed ();
    }

    p->src_root = d.leaf (); // For now assuming to be in configuration.
    p->purge_src = true;

    p->state = package_state::unpacked;

    db.update (p);
    t.commit ();

    arm.cancel ();

    return p;
  }

  int
  pkg_unpack (const pkg_unpack_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_unpack");

    if (o.replace () && !o.existing ())
      fail << "--replace|-r can only be specified with --existing|-e";

    const dir_path& c (o.directory ());
    l4 ([&]{trace << "configuration: " << c;});

    database db (open (c, trace));
    transaction t (db.begin ());

    shared_ptr<selected_package> p;

    if (o.existing ())
    {
      // The package directory case.
      //
      if (!args.more ())
        fail << "package directory argument expected" <<
          info << "run 'bpkg help pkg-unpack' for more information";

      p = pkg_unpack (
        c, t, dir_path (args.next ()), o.replace (), o.purge ());
    }
    else
    {
      // The package name case.
      //
      if (!args.more ())
        fail << "package name argument expected" <<
          info << "run 'bpkg help pkg-unpack' for more information";

      p = pkg_unpack (o, c, t, args.next ());
    }

    if (verb)
      text << "unpacked " << p->name << " " << p->version;

    return 0;
  }
}
