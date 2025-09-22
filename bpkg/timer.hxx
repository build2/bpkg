#pragma once

#include <time.h>

#include <vector>
#include <string>
#include <utility> // pair
#include <cstddef> // size_t
#include <cstdint> // uint64_t

class timer
{
public:
  timer (size_t id,
         const char* name,
         bool start = true,
         bool recursive = false);

  ~timer () {stop ();}

  void
  start ();

  void
  stop ();

  static void
  print (bool total_count = false, bool id = false);

private:
  size_t id_;

  struct timer_data
  {
    const char* name = nullptr;

    std::uint64_t count = 0;
    timespec time = {0, 0};

    std::uint64_t started = 0;
    timespec start_time = {0, 0};

    bool recursive = false;
  };

  static std::vector<timer_data> timers;

  static std::uint64_t total_count;
  static timespec total_time;
};

inline timespec
operator+= (timespec& x, timespec y)
{
  x.tv_sec += y.tv_sec;
  x.tv_nsec += y.tv_nsec;

  if (x.tv_nsec >= 1000000000)
  {
    x.tv_nsec -= 1000000000;
    ++x.tv_sec;
  }

  return x;
}

inline timespec
operator+ (timespec x, timespec y)
{
  return (x += y);
}

inline timespec
operator-= (timespec& x, timespec y)
{
  x.tv_sec -= y.tv_sec;

  if (x.tv_nsec < y.tv_nsec)
  {
    x.tv_nsec += 1000000000;
    --x.tv_sec;
  }

  x.tv_nsec -= y.tv_nsec;

  return x;
}

inline timespec
operator- (timespec x, timespec y)
{
  return (x -= y);
}
