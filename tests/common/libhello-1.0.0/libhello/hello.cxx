// file: libhello/hello.cxx -*- C++ -*-

#include <libhello/hello.hxx>

#include <iostream>

using namespace std;

namespace hello
{
  void
  say (const string& n)
  {
    cout << "Hello, " << n << '!' << endl;
  }
}
