// file      : bpkg/utility.txx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/diagnostics.hxx>

namespace bpkg
{
  // *_b()
  //
  template <typename O, typename E, typename... A>
  process
  start_b (const common_options& co,
           O&& out,
           E&& err,
           verb_b v,
           A&&... args)
  {
    const char* b (name_b (co));

    try
    {
      // Use our executable directory as a fallback search since normally the
      // entire toolchain is installed into one directory. This way, for
      // example, if we installed into /opt/build2 and run bpkg with absolute
      // path (and without PATH), then bpkg will be able to find "its" b.
      //
      process_path pp (process::path_search (b, exec_dir));

      small_vector<const char*, 1> ops;

      // Map verbosity level. If we are running quiet or at level 1,
      // then run build2 quiet. Otherwise, run it at the same level
      // as us.
      //
      string vl;
      bool no_progress (co.no_progress ());

      if (verb == 0)
      {
        ops.push_back ("-q");
        no_progress = false;  // Already suppressed with -q.
      }
      else if (verb == 1)
      {
        if (v != verb_b::normal)
        {
          ops.push_back ("-q");

          if (!no_progress)
          {
            if (v == verb_b::progress && stderr_term)
              ops.push_back ("--progress");
          }
          else
            no_progress = false; // Already suppressed with -q.
        }
      }
      else if (verb == 2)
        ops.push_back ("-v");
      else
      {
        vl = to_string (verb);
        ops.push_back ("--verbose");
        ops.push_back (vl.c_str ());
      }

      if (no_progress)
        ops.push_back ("--no-progress");

      return process_start_callback (
        [] (const char* const args[], size_t n)
        {
          if (verb >= 2)
            print_process (args, n);
        },
        0 /* stdin */,
        forward<O> (out),
        forward<E> (err),
        pp,
        ops,
        co.build_option (),
        forward<A> (args)...);
    }
    catch (const process_error& e)
    {
      fail << "unable to execute " << b << ": " << e << endf;
    }
  }

  template <typename... A>
  void
  run_b (const common_options& co, verb_b v, A&&... args)
  {
    process pr (
      start_b (co, 1 /* stdout */, 2 /* stderr */, v, forward<A> (args)...));

    if (!pr.wait ())
    {
      const process_exit& e (*pr.exit);

      if (e.normal ())
        throw failed (); // Assume the child issued diagnostics.

      fail << "process " << name_b (co) << " " << e;
    }
  }
}
