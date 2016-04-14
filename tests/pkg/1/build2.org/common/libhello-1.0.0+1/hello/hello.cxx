// file: hello/hello.cxx -*- C++ -*-

#include <hello/hello>

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
