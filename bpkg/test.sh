#! /usr/bin/env bash

trap 'exit 1' ERR

bpkg="./bpkg $*"
#bpkg="valgrind -q ./bpkg $*"
cfg=/tmp/conf
pkg=libhello
ver=1.0.0
pkga=../../hello/dist/$pkg-$ver.tar.bz2
pkgd=../../hello/dist/$pkg-$ver

function error ()
{
  echo "$*" 1>&2
  exit 1
}

function test ()
{
  local cmd=$1
  shift

  $bpkg $cmd -d $cfg $*

  if [ $? -ne 0 ]; then
    error "failed: $bpkg $cmd -d $cfg $*"
  fi
}

function fail ()
{
  local cmd=$1
  shift

  $bpkg $cmd -d $cfg $*

  if [ $? -eq 0 ]; then
    error "succeeded: $bpkg $cmd -d $cfg $*"
  fi

  return 0
}

# Verify package status.
#
function stat ()
{
  local s=`$bpkg pkg-status -d $cfg $pkg $ver`

  if [ "$s" != "$1" ]; then
    error "status: $s, expected: $1"
  fi
}

# Verify path is gone (no longer exists)
#
function gone ()
{
  if [ -e "$1" ]; then
    error "path $1 still exists"
  fi
}

##
## cfg-create
##

test cfg-create --wipe config.cxx=g++-4.9 cxx config.install.root=/tmp/install
stat unknown

##
## pkg-fetch
##

# fetch existing archive
#
stat unknown
test pkg-fetch -e $pkga
stat fetched
test pkg-purge $pkg
stat unknown


##
## pkg-purge
##

fail pkg-purge
fail pkg-purge $pkg

# purge fetched
#
test pkg-fetch -e $pkga
test pkg-purge $pkg
stat unknown

# --keep
#
test pkg-fetch -e $pkga
test pkg-purge -k $pkg
stat fetched
test pkg-purge $pkg

# archive --purge
#
cp $pkga $cfg/
test pkg-fetch -e -p $cfg/`basename $pkga`
test pkg-purge $pkg
stat unknown
gone $cfg/`basename $pkga`

# no archive but --keep
#
test pkg-unpack -e $pkgd
fail pkg-purge --keep $pkg
stat unpacked
test pkg-purge $pkg

# purge unpacked directory
#
test pkg-unpack -e $pkgd
test pkg-purge $pkg
stat unknown

# purge unpacked archive
#
test pkg-fetch -e $pkga
test pkg-unpack $pkg
test pkg-purge $pkg
stat unknown
gone $cfg/`basename $pkgd`

# purge unpacked archive but --keep
#
test pkg-fetch -e $pkga
test pkg-unpack $pkg
test pkg-purge --keep $pkg
stat fetched
gone $cfg/`basename $pkgd`
test pkg-purge $pkg

# directory --purge
#
cp -r $pkgd $cfg/
test pkg-unpack -e -p $cfg/`basename $pkgd`
test pkg-purge $pkg
stat unknown
gone $cfg/`basename $pkgd`

# archive --purge
#
cp $pkga $cfg/
test pkg-fetch -e -p $cfg/`basename $pkga`
test pkg-unpack $pkg
test pkg-purge $pkg
stat unknown
gone $cfg/`basename $pkgd`
gone $cfg/`basename $pkga`

# broken
#
cp $pkga $cfg/
test pkg-fetch -e -p $cfg/`basename $pkga`
test pkg-unpack $pkg
chmod 000 $cfg/`basename $pkgd`
fail pkg-purge $pkg
stat broken
fail pkg-purge $pkg        # need --force
fail pkg-purge -f -k $pkg  # can't keep broken
fail pkg-purge -f $pkg     # directory still exists
chmod 755 $cfg/`basename $pkgd`
rm -r $cfg/`basename $pkgd`
fail pkg-purge -f $pkg     # archive still exists
rm $cfg/`basename $pkga`
test pkg-purge -f $pkg
stat unknown
