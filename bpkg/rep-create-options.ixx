// -*- C++ -*-
//
// This file was generated by CLI, a command line interface
// compiler for C++.
//

// Begin prologue.
//
//
// End prologue.

namespace bpkg
{
  // rep_create_options
  //

  inline const bool& rep_create_options::
  ignore_unknown () const
  {
    return this->ignore_unknown_;
  }

  inline const butl::standard_version& rep_create_options::
  min_bpkg_version () const
  {
    return this->min_bpkg_version_;
  }

  inline bool rep_create_options::
  min_bpkg_version_specified () const
  {
    return this->min_bpkg_version_specified_;
  }

  inline const string& rep_create_options::
  key () const
  {
    return this->key_;
  }

  inline bool rep_create_options::
  key_specified () const
  {
    return this->key_specified_;
  }
}

// Begin epilogue.
//
//
// End epilogue.
