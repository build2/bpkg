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
timer (size_t id, const char* name, bool s)
    : id_ (id)
{
  assert (id < timers.size () && name != nullptr);

  timer_data& t (timers[id_]);

  assert (t.name == nullptr || t.name == name || strcmp (t.name, name) == 0);

  t.name = name;

  if (s)
    start ();
}

void timer::
start ()
{
  timer_data& t (timers[id_]);

  assert (!t.started && t.name != nullptr);

  if (clock_gettime (CLOCK_MONOTONIC, &t.start_time) == -1)
  {
    cerr << "timer: unable to get current time" << endl;
    abort();
  }

  t.started = true;
}

void timer::
stop ()
{
  timer_data& t (timers[id_]);

  assert (t.name != nullptr);

  if (t.started)
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

    t.started = false;
  }
}

void timer::
print ()
{
  cerr << "total (sec) name\n";

  for (const timer_data& t: timers)
  {
    if (t.name != nullptr)
    {
      const timespec& tm (t.time);
      cerr << tm.tv_sec << '.';

      ostream::fmtflags fl (cerr.flags ());
      char fc (cerr.fill ('0'));

      cerr << setw (9) << tm.tv_nsec;

      cerr.fill (fc);
      cerr.flags (fl);

      cerr << ' ' << t.name << endl;
    }
  }
}
