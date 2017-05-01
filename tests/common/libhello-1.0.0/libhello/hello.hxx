// file: libhello/hello.hxx -*- C++ -*-

#pragma once

#include <string>

#include <libhello/export.hxx>

namespace hello
{
  LIBHELLO_EXPORT void
  say (const std::string& name);
}
