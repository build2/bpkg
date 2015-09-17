// file      : bpkg/pkg-unpack.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/pkg-unpack>

#include <memory> // shared_ptr

#include <butl/process>

#include <bpkg/manifest>

#include <bpkg/types>
#include <bpkg/package>
#include <bpkg/package-odb>
#include <bpkg/utility>
#include <bpkg/database>
#include <bpkg/diagnostics>

#include <bpkg/pkg-verify>

using namespace std;
using namespace butl;

namespace bpkg
{
  static shared_ptr<package>
  pkg_unpack (database& db, const dir_path& c, const dir_path& d, bool purge)
  {
    tracer trace ("pkg_unpack(dir)");
    db.tracer (trace); // "Tail" call, never restored.

    if (!exists (d))
      fail << "package directory " << d << " does not exist";

    // Verify the directory is a package and get its manifest.
    //
    package_manifest m (pkg_verify (d));
    level4 ([&]{trace << d << ": " << m.name << " " << m.version;});

    const auto& n (m.name);

    transaction t (db.begin ());

    // See if this package already exists in this configuration.
    //
    if (shared_ptr<package> p = db.find<package> (n))
      fail << "package " << n << " already exists in configuration " << c <<
        info << "version: " << p->version << ", state: " << p->state;

    // Make the package and configuration paths absolute and normalized.
    // If the package is inside the configuration, use the relative path.
    // This way we can move the configuration around.
    //
    dir_path ac (c), ad (d);
    ac.complete ().normalize ();
    ad.complete ().normalize ();

    if (ad.sub (ac))
      ad = ad.leaf (ac);

    // Add the package to the configuration.
    //
    shared_ptr<package> p (new package {
        move (m.name),
        move (m.version),
        state::unpacked,
        optional<path> (),    // No archive
        false,                // Don't purge archive.
        move (ad),
        purge,
        optional<dir_path> () // No output directory yet.
     });

    db.persist (p);
    t.commit ();

    return p;
  }

  static shared_ptr<package>
  pkg_unpack (database& db, const dir_path& c, const string& name)
  {
    tracer trace ("pkg_unpack(pkg)");
    db.tracer (trace); // "Tail" call, never restored.

    transaction t (db.begin ());
    shared_ptr<package> p (db.find<package> (name));

    if (p == nullptr)
      fail << "package " << name << " does not exist in configuration " << c;

    if (p->state != state::fetched)
      fail << "package " << name << " is " << p->state <<
        info << "expected it to be fetched";

    level4 ([&]{trace << p->name << " " << p->version;});

    assert (p->archive); // Should have archive in the fetched state.

    // If the archive path is not absolute, then it must be relative
    // to the configuration.
    //
    path a (p->archive->absolute () ? *p->archive : c / *p->archive);

    level4 ([&]{trace << "archive: " << a;});

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

    const char* args[] {
      "tar",
      "-C", c.string ().c_str (), // -C/--directory -- change to directory.
      "-xf",
      a.string ().c_str (),
      nullptr};

    if (verb >= 2)
      print_process (args);

    // What should we do if tar or something after it fails? Cleaning
    // up the package directory sounds like the right thing to do.
    //
    auto dg (
      make_exception_guard (
        [&d]()
        {
          if (exists (d))
            rm_r (d);
        }));

    try
    {
      process pr (args);

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

    p->state = state::unpacked;

    db.update (p);
    t.commit ();

    return p;
  }

  void
  pkg_unpack (const pkg_unpack_options& o, cli::scanner& args)
  {
    tracer trace ("pkg_unpack");

    const dir_path& c (o.directory ());
    level4 ([&]{trace << "configuration: " << c;});

    database db (open (c, trace));

    shared_ptr<package> p;

    if (o.existing ())
    {
      // The package directory case.
      //
      if (!args.more ())
        fail << "package directory argument expected" <<
          info << "run 'bpkg help pkg-unpack' for more information";

      p = pkg_unpack (db, c, dir_path (args.next ()), o.purge ());
    }
    else
    {
      // The package name case.
      //
      if (!args.more ())
        fail << "package name argument expected" <<
          info << "run 'bpkg help pkg-unpack' for more information";

      p = pkg_unpack (db, c, args.next ());
    }

    if (verb)
      text << "unpacked " << p->name << " " << p->version;
  }
}
