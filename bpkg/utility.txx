// file      : bpkg/utility.txx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/diagnostics.hxx>

namespace bpkg
{
  // *_b()
  //
  template <typename V>
  void
  map_verb_b (const common_options& co, verb_b v, V& ops, string& verb_arg)
  {
    // Map verbosity level. If we are running quiet or at level 1,
    // then run build2 quiet. Otherwise, run it at the same level
    // as us.
    //
    bool progress    (co.progress ());
    bool no_progress (co.no_progress ());

    if (verb == 0)
    {
      ops.push_back ("-q");
      no_progress = false;  // Already suppressed with -q.
    }
    else if (verb == 1)
    {
      // NOTE: search for verb_b usage if changing anything here.
      //
      if (v != verb_b::normal)
      {
        ops.push_back ("-q");

        if (!no_progress)
        {
          if (v == verb_b::progress && stderr_term)
          {
            ops.push_back ("--progress");
              progress = false; // The option is already added.
          }
        }
        else
          no_progress = false; // Already suppressed with -q.
      }
    }
    else if (verb == 2)
      ops.push_back ("-v");
    else
    {
      verb_arg = to_string (verb);
      ops.push_back ("--verbose");
      ops.push_back (verb_arg.c_str ());
    }

    if (progress)
      ops.push_back ("--progress");

    if (no_progress)
      ops.push_back ("--no-progress");
  }

  template <typename... A>
  void
  print_b (const common_options& co, verb_b v, A&&... args)
  {
    process_path pp (search_b (co));

    small_vector<const char*, 1> ops;

    // As in start_b() below.
    //
    string verb_arg;
    map_verb_b (co, v, ops, verb_arg);

    if (co.diag_color ())
      ops.push_back ("--diag-color");

    if (co.no_diag_color ())
      ops.push_back ("--no-diag-color");

    process_print_callback (
      [] (const char* const args[], size_t n)
      {
        print_process (args, n);
      },
      pp,
      ops,
      co.build_option (),
      forward<A> (args)...);
  }

  template <typename O, typename E, typename... A>
  process
  start_b (const common_options& co,
           O&& out,
           E&& err,
           verb_b v,
           A&&... args)
  {
    process_path pp (search_b (co));

    try
    {
      small_vector<const char*, 1> ops;

      // NOTE: see print_b() above if changing anything here.
      //
      // NOTE: see custom versions in system_package_manager* if adding
      //       anything new here (search for search_b()).

      string verb_arg;
      map_verb_b (co, v, ops, verb_arg);

      // Forward our --[no]diag-color options.
      //
      if (co.diag_color ())
        ops.push_back ("--diag-color");

      if (co.no_diag_color ())
        ops.push_back ("--no-diag-color");

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
      fail << "unable to execute " << pp.recall_string () << ": " << e << endf;
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
