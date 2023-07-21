// file      : bpkg/satisfaction.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/satisfaction.hxx>

#include <bpkg/package-odb.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;

namespace bpkg
{
  bool
  satisfies (const version& v, const version_constraint& c)
  {
    assert (!c.empty () && c.complete ());

    if (v == wildcard_version)
      return true;

    bool s (true);

    // Here an absent revision means zero revision and version X must satisfy
    // the [X+0 ...) version constraint. Note that technically X < X+0.
    //
    version ev (v.epoch,
                v.upstream,
                v.release,
                v.effective_revision (),
                v.iteration);

    // See notes in pkg-build:query_available() on ignoring revision in
    // comparison.
    //
    if (c.min_version)
    {
      int i (ev.compare (*c.min_version,
                         !c.min_version->revision,
                         true /* ignore_iteration */));

      s = c.min_open ? i > 0 : i >= 0;
    }

    if (s && c.max_version)
    {
      int i (ev.compare (*c.max_version,
                         !c.max_version->revision,
                         true /* ignore_iteration */));

      s = c.max_open ? i < 0 : i <= 0;
    }

    return s;
  }

  bool
  satisfies (const version_constraint& l, const version_constraint& r)
  {
    assert (!l.empty () && l.complete () && !r.empty () && r.complete ());

    // Note that a revision should not be ignored if we compare the endpoint
    // versions. However, an absent revision translates into the effective
    // revision differently, depending on the range endpoint side and openness
    // (see libbpkg/manifest.hxx for details). That's why we normalize
    // endpoint versions prior to comparison.
    //
    auto norm = [] (const version& v, bool min, bool open)
    {
      // Return the version as is if the revision is present or this is an
      // earliest release (for which the revision is meaningless).
      //
      // We could probably avoid copying of versions that don't require
      // normalization but let's keep it simple for now.
      //
      if (v.revision || (v.release && v.release->empty ()))
        return v;

      return version (v.epoch,
                      v.upstream,
                      v.release,
                      (min && !open) || (!min && open) ? 0 : uint16_t (~0),
                      v.iteration);
    };

    bool s (false);

    if (l.min_version)
    {
      if (r.min_version)
      {
        version lv (norm (*l.min_version, true /* min */, l.min_open));
        version rv (norm (*r.min_version, true /* min */, r.min_open));

        int i (lv.compare (rv,
                           false /* ignore_revision */,
                           true  /* ignore_iteration */));

        if (l.min_open)
          // Doesn't matter if r is min_open or not.
          //
          s = i >= 0;
        else
          s = r.min_open ? i > 0 : i >= 0;
      }
      else
        s = true; // Doesn't matter what l.min_version is.
    }
    else
      s = !r.min_version;

    if (s)
    {
      if (l.max_version)
      {
        if (r.max_version)
        {
          version lv (norm (*l.max_version, false /* min */, l.max_open));
          version rv (norm (*r.max_version, false /* min */, r.max_open));

          int i (lv.compare (rv,
                             false /* ignore_revision */,
                             true  /* ignore_iteration */));

          if (l.max_open)
            // Doesn't matter if r is max_open or not.
            //
            s = i <= 0;
          else
            s = r.max_open ? i < 0 : i <= 0;
        }
        else
        {
          // Doesn't matter what l.max_version is, so leave s to be true.
        }
      }
      else
        s = !r.max_version;
    }

    return s;
  }

  version build2_version;

  bool
  satisfy_build2 (const common_options& o, const dependency& d)
  {
    assert (d.name == "build2");

    // Extract, parse, and cache build2 version string.
    //
    if (build2_version.empty ())
    {
      fdpipe pipe (open_pipe ());

      process pr (start_b (o,
                           pipe, 2 /* stderr */,
                           verb_b::quiet,
                           "--version"));

      // Shouldn't throw, unless something is severely damaged.
      //
      pipe.out.close ();

      string l;
      try
      {
        ifdstream is (move (pipe.in), fdstream_mode::skip);
        getline (is, l);
        is.close ();

        if (pr.wait () && l.compare (0, 7, "build2 ") == 0)
        {
          try
          {
            build2_version = version (string (l, 7));
          }
          catch (const invalid_argument&) {} // Fall through.
        }

        // Fall through.
      }
      catch (const io_error&)
      {
        pr.wait ();
        // Fall through.
      }

      if (build2_version.empty ())
        fail << "unable to determine build2 version of " << name_b (o);
    }

    return satisfies (build2_version, d.constraint);
  }

  version bpkg_version;

  bool
  satisfy_bpkg (const common_options&, const dependency& d)
  {
    assert (d.name == "bpkg");

    // Parse and cache bpkg version string.
    //
    if (bpkg_version.empty ())
      bpkg_version = version (BPKG_VERSION_STR);

    return satisfies (bpkg_version, d.constraint);
  }
}
