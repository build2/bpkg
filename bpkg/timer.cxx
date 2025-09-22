#include "timer.hxx"

#include <cstring>
#include <iomanip>  // setw(), right
#include <cstdlib>
#include <cassert>  // assert()
#include <iostream>

using namespace std;

vector<timer::timer_data> timer::timers (10000);

uint64_t timer::total_count (0);
timespec timer::total_time {0, 0};

timer::
timer (size_t id, const char* name, bool s, bool r)
    : id_ (id)
{
  assert (id < timers.size () && name != nullptr);

  timer_data& t (timers[id_]);

  if (t.name != nullptr && t.name != name && strcmp (t.name, name) != 0)
    cerr << "timer: cannot add timer '" << name << "' (" << id_ << "): "
         << "already exists with name '" << t.name << "'" << endl;

  assert (t.name == nullptr || t.name == name || strcmp (t.name, name) == 0);

  t.name = name;
  t.recursive = r;

  if (s)
    start ();
}

void timer::
start ()
{
  timer_data& t (timers[id_]);

  if (t.name == nullptr)
    cerr << "timer: unnamed timer " << id_ << endl;

  assert (t.name != nullptr);

  if (t.started != 0 && !t.recursive)
    cerr << "timer: timer '" << t.name << "' (" << id_ << ") already started"
         << endl;

  assert (t.started == 0 || t.recursive);

  if (t.started == 0)
  {
    if (clock_gettime (CLOCK_MONOTONIC, &t.start_time) == -1)
    {
      cerr << "timer: unable to get current time" << endl;
      abort();
    }
  }

  t.started++;
}

void timer::
stop ()
{
  timer_data& t (timers[id_]);

  assert (t.name != nullptr);

  if (t.started != 0 && --t.started == 0)
  {
    timespec stop_time;

    if (clock_gettime (CLOCK_MONOTONIC, &stop_time) == -1)
    {
      cerr << "timer: unable to get current time" << endl;
      abort();
    }

    timespec d (stop_time - t.start_time);

    t.time += d;
    ++t.count;

    total_time += d;
    ++total_count;
  }
}

void timer::
print (bool total_count, bool id)
{
  cerr << "  total (sec)";

  if (total_count)
    cerr << "   count";

  cerr << " name\n";

  for (size_t i (0); i != timers.size (); ++i)
  {
    const timer_data& t (timers[i]);

    if (t.name != nullptr)
    {
      const timespec& tm (t.time);
      cerr << setw (3) << tm.tv_sec << '.';

      ostream::fmtflags fl (cerr.flags ());
      char fc (cerr.fill ('0'));

      cerr << setw (9) << tm.tv_nsec;

      cerr.fill (fc);
      cerr.flags (fl);

      if (total_count)
        cerr << ' ' << setw (7) << t.count;

      cerr << ' ' << t.name;

      if (id)
        cerr << " [" << i << ']';

      cerr << endl;
    }
  }
}
