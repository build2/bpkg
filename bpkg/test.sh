#! /usr/bin/env bash

# -v Run verbose. By default, this script runs bpkg quiet and suppresses
#    error messages in the fail tests. Note that when this options is
#    specified, bpkg is called with default verbosity level. If you want
#    more bpkg diagnostics, add the --verbose N option.
#

trap 'exit 1' ERR

function error ()
{
  echo "$*" 1>&2
  exit 1
}

bpkg="./bpkg"
#bpkg="valgrind -q ./bpkg"
#bpkg="./bpkg --fetch curl"
#bpkg="./bpkg --fetch fetch --tar bsdtar"
cfg=/tmp/conf
pkg=libhello
ver=1.0.0
pkga=../../hello/dist/$pkg-$ver.tar.bz2
pkgd=../../hello/dist/$pkg-$ver
out=$cfg/`basename $pkgd`
rep=../../hello/1/hello

abs_rep=`realpath ../tests/repository/1`

verbose=n
options=

while [ $# -gt 0 ]; do
  case $1 in
    -v)
      verbose=y
      shift
      ;;
    *)
      options="$options $1"
      shift
      ;;
  esac
done

if [ "$verbose" != "y" ]; then
  options="$options -q"
fi

bpkg="$bpkg $options"

#
#
function test ()
{
  local cmd=$1; shift
  local ops=

  if [ "$cmd" != "rep-create" -a "$cmd" != "rep-info" ]; then
    ops="-d $cfg"
  fi

  if [ -t 0 ]; then
    $bpkg $cmd $ops $*
  else
    # There is no way to get the exit code in process substitution
    # so ruin the output.
    #
    diff -u - <($bpkg $cmd $ops $* || echo "<invalid output>")
  fi

  if [ $? -ne 0 ]; then
    error "failed: $bpkg $cmd $ops $*"
  fi
}

function fail ()
{
  local cmd=$1; shift
  local ops=

  if [ "$cmd" != "rep-create" -a "$cmd" != "rep-info" ]; then
    ops="-d $cfg"
  fi

  if [ "$verbose" = "y" ]; then
    $bpkg $cmd $ops $*
  else
    $bpkg $cmd $ops $* 2>/dev/null
  fi

  if [ $? -eq 0 ]; then
    error "succeeded: $bpkg $cmd $ops $*"
  fi

  return 0
}

# Verify package status.
#
function stat ()
{
  local c="$bpkg pkg-status -d $cfg"

  if [ $# -eq 1 ]; then
    c="$c $pkg/$ver"
  elif [ $# -eq 2 ]; then
    c="$c $1"; shift
  fi

  local s=`$c`

  if [ "$s" != "$1" ]; then
    error "status: '"$s"', expected: '"$1"'"
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
## rep-create
##

fail rep-create # no 'repositories' file

test rep-create ../tests/repository/1/misc/stable
test rep-create ../tests/repository/1/misc/testing

test rep-create ../tests/repository/1/math/stable
test rep-create ../tests/repository/1/math/testing
test rep-create ../tests/repository/1/math/unstable

##
## rep-info
##

fail rep-info # repository location expected

test rep-info ../tests/repository/1/misc/testing <<EOF
misc/testing $abs_rep/misc/testing
complement misc/stable $abs_rep/misc/stable
libhello 1.0.0-1
EOF

test rep-info -m ../tests/repository/1/math/unstable <<EOF
math/unstable $abs_rep/math/unstable
: 1
location: ../../misc/testing
:
location: ../testing
role: complement
:
EOF

test rep-info http://pkg.cppget.org/1/hello <<EOF
cppget.org/hello http://pkg.cppget.org/1/hello
libheavy 1.0.0
libhello 1.0.0
EOF

##
## cfg-create
##

test cfg-create --wipe config.cxx=g++-4.9 cxx config.install.root=/tmp/install
stat unknown

##
## rep-add
##

test cfg-create --wipe

fail rep-add         # repository location expected
fail rep-add stable  # invalid location
fail rep-add http:// # invalid location

# relative path
#
test rep-add ./1/math/stable
fail rep-add ./1/../1/math/stable # duplicate

# absolute path
#
test rep-add /tmp/1/misc/stable
fail rep-add /tmp/1/../1/misc/stable # duplicate

# remote URL
#
test rep-add http://pkg.example.org/1/testing
fail rep-add http://www.example.org/1/testing # duplicate

##
## rep-fetch
##

test cfg-create --wipe

fail rep-fetch # no repositories

# hello repository
#
test cfg-create --wipe
test rep-add $rep
test rep-fetch
test rep-fetch

# math/unstable repository
#
test cfg-create --wipe
test rep-add ../tests/repository/1/math/unstable
test rep-fetch
test rep-fetch

# both
#
test cfg-create --wipe
test rep-add $rep
test rep-add ../tests/repository/1/math/unstable
test rep-fetch
test rep-fetch

# remote
#
test cfg-create --wipe
test rep-add http://pkg.cppget.org/1/hello
test rep-fetch

##
## pkg-fetch
##
test rep-create ../tests/repository/1/fetch/t1
test cfg-create --wipe

fail pkg-fetch -e                # archive expected
fail pkg-fetch -e ./no-such-file # archive does not exist

fail pkg-fetch                   # package name expected
fail pkg-fetch libfoo            # package version expected
fail pkg-fetch libfoo/1/2/3      # invalid package version

fail pkg-fetch libfoo/1.0.0      # no repositories
test rep-add ../tests/repository/1/fetch/t1
fail pkg-fetch libfoo/1.0.0      # no packages
test rep-fetch
fail pkg-fetch libfoo/2+1.0.0    # not available

# local
#
test cfg-create --wipe
test rep-add ../tests/repository/1/fetch/t1
test rep-fetch
test pkg-fetch libfoo/1.0.0
stat libfoo/1.0.0 fetched
fail pkg-fetch libfoo/1.0.0
fail pkg-fetch -e ../tests/repository/1/fetch/t1/libfoo-1.0.0.tar.gz
test pkg-purge libfoo
test pkg-fetch -e ../tests/repository/1/fetch/t1/libfoo-1.0.0.tar.gz
stat libfoo/1.0.0 fetched
test pkg-unpack libfoo
test pkg-fetch -r libfoo/1.1.0
stat libfoo/1.1.0 fetched
test pkg-unpack libfoo
test pkg-fetch -r -e ../tests/repository/1/fetch/t1/libfoo-1.0.0.tar.gz
stat libfoo/1.0.0 fetched
test pkg-fetch -r libfoo/1.1.0
stat libfoo/1.1.0 fetched
test pkg-fetch -r -e ../tests/repository/1/fetch/t1/libfoo-1.0.0.tar.gz
stat libfoo/1.0.0 fetched
test pkg-purge libfoo

# remote
#
test cfg-create --wipe
test rep-add http://pkg.cppget.org/1/hello
test rep-fetch
#test pkg-fetch libheavy/1.0.0
test pkg-fetch libhello/1.0.0
test pkg-unpack libhello
test pkg-purge libhello

## @@
##
##

test cfg-create --wipe config.cxx=g++-4.9 cxx config.install.root=/tmp/install
stat unknown

# fetch existing archive
#
stat unknown
test pkg-fetch -e $pkga
stat fetched
test pkg-purge $pkg
stat unknown

##
## pkg-unpack
##

# @@ TODO

# replace
#
test cfg-create --wipe
test rep-add ../tests/repository/1/fetch/t1
test rep-fetch
test pkg-fetch libfoo/1.0.0
fail pkg-unpack -e ../tests/repository/1/fetch/libfoo-1.1.0
test pkg-unpack -r -e ../tests/repository/1/fetch/libfoo-1.1.0
stat libfoo/1.1.0 unpacked
test pkg-purge libfoo
test pkg-fetch libfoo/1.0.0
test pkg-unpack libfoo
fail pkg-unpack -e ../tests/repository/1/fetch/libfoo-1.1.0
test pkg-unpack -r -e ../tests/repository/1/fetch/libfoo-1.1.0
stat libfoo/1.1.0 unpacked
test pkg-purge libfoo


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
gone $out

# purge unpacked archive but --keep
#
test pkg-fetch -e $pkga
test pkg-unpack $pkg
test pkg-purge --keep $pkg
stat fetched
gone $out
test pkg-purge $pkg

# directory --purge
#
cp -r $pkgd $cfg/
test pkg-unpack -e -p $out
test pkg-purge $pkg
stat unknown
gone $out

# archive --purge
#
cp $pkga $cfg/
test pkg-fetch -e -p $cfg/`basename $pkga`
test pkg-unpack $pkg
test pkg-purge $pkg
stat unknown
gone $out
gone $cfg/`basename $pkga`

# broken
#
cp $pkga $cfg/
test pkg-fetch -e -p $cfg/`basename $pkga`
test pkg-unpack $pkg
chmod 000 $out
fail pkg-purge $pkg
stat broken
fail pkg-purge $pkg        # need --force
fail pkg-purge -f -k $pkg  # can't keep broken
fail pkg-purge -f $pkg     # directory still exists
chmod 755 $out
rm -r $out
fail pkg-purge -f $pkg     # archive still exists
rm $cfg/`basename $pkga`
test pkg-purge -f $pkg
stat unknown

##
## pkg-configure/pkg-disfigure
##

fail pkg-configure                        # package name expected
fail pkg-configure config.dist.root=/tmp  # ditto
fail pkg-configure $pkg $pkg              # unexpected argument
fail pkg-configure $pkg                   # no such package

fail pkg-disfigure                        # package name expected
fail pkg-disfigure $pkg                   # no such package

test pkg-fetch -e $pkga

fail pkg-configure $pkg                   # wrong package state
fail pkg-disfigure $pkg                   # wrong package state

test pkg-purge $pkg

# src == out
#
test pkg-fetch -e $pkga
test pkg-unpack $pkg
test pkg-configure $pkg
stat configured
test pkg-disfigure $pkg
stat unpacked
test pkg-purge $pkg
stat unknown

# src != out
#
test pkg-unpack -e $pkgd
test pkg-configure $pkg
stat configured
test pkg-disfigure $pkg
stat unpacked
test pkg-purge $pkg
stat unknown
gone $out

# out still exists after disfigure
#
test pkg-unpack -e $pkgd
test pkg-configure $pkg
touch $out/stray
fail pkg-disfigure $pkg
stat broken
rm -r $out
test pkg-purge -f $pkg
stat unknown

# disfigure failed
#
test pkg-unpack -e $pkgd
test pkg-configure $pkg
chmod 555 $out
fail pkg-disfigure $pkg
stat broken
chmod 755 $out
rm -r $out
test pkg-purge -f $pkg
stat unknown

# configure failed but disfigure succeeds
#
test pkg-unpack -e $pkgd
mkdir -p $out/build
chmod 555 $out/build
fail pkg-configure $pkg
stat unpacked
test pkg-purge $pkg
stat unknown

# configure and disfigure both failed
#
test pkg-unpack -e $pkgd
mkdir -p $out/build
chmod 555 $out $out/build # Both to trip configure and disfigure.
fail pkg-configure $pkg
stat broken
chmod 755 $out $out/build
rm -r $out
test pkg-purge -f $pkg
stat unknown

# dependency management
#
test rep-create ../tests/repository/1/depend/stable
test cfg-create --wipe
test rep-add ../tests/repository/1/depend/stable
test rep-fetch

test pkg-fetch libbar/1.0.0
test pkg-unpack libbar
fail pkg-configure libbar # no libfoo
stat libbar/1.0.0 "unpacked"
test pkg-fetch libfoo/1.0.0
test pkg-unpack libfoo
fail pkg-configure libbar # libfoo not configured
test pkg-configure libfoo
test pkg-configure libbar
fail pkg-disfigure libfoo # libbar still depends on libfoo
test pkg-disfigure libbar
test pkg-disfigure libfoo
test pkg-purge libbar
test pkg-purge libfoo

test pkg-fetch libfoo/1.0.0
test pkg-unpack libfoo
test pkg-configure libfoo
test pkg-fetch libbar/1.1.0
test pkg-unpack libbar
fail pkg-configure libbar # libfoo >= 1.1.0
test pkg-disfigure libfoo
test pkg-purge libfoo
test pkg-fetch libfoo/1.1.0
test pkg-unpack libfoo
test pkg-configure libfoo
test pkg-configure libbar
test pkg-disfigure libbar
test pkg-disfigure libfoo
test pkg-purge libfoo
test pkg-purge libbar

test pkg-fetch libfoo/1.1.0
test pkg-unpack libfoo
test pkg-configure libfoo
test pkg-fetch libbar/1.2.0
test pkg-unpack libbar
fail pkg-configure libbar # libfoo >= 1.2.0
test pkg-disfigure libfoo
test pkg-purge libfoo
test pkg-fetch libfoo/1.2.0
test pkg-unpack libfoo
test pkg-configure libfoo
test pkg-configure libbar
fail pkg-disfigure libfoo # "package libbar on libfoo >= 1.2.0"
test pkg-disfigure libbar
test pkg-disfigure libfoo
test pkg-purge libfoo
test pkg-purge libbar

test pkg-fetch libfoo/1.1.0
test pkg-unpack libfoo
test pkg-configure libfoo
test pkg-fetch libbar/1.3.0
test pkg-unpack libbar
fail pkg-configure libbar # incompatible constraints
test pkg-disfigure libfoo
test pkg-purge libfoo
test pkg-purge libbar

##
## pkg-status (also tested in pkg-{fetch,unpack,configure,disfigure,purge}
##

test rep-create ../tests/repository/1/status/stable
test rep-create ../tests/repository/1/status/extra
test rep-create ../tests/repository/1/status/testing
test rep-create ../tests/repository/1/status/unstable

# basics
#
test cfg-create --wipe
stat libfoo/1.0.0 "unknown"
stat libfoo "unknown"
test rep-add ../tests/repository/1/status/stable
test rep-fetch
stat libfoo/1.0.0 "available"
stat libfoo "available 1.0.0"
test pkg-fetch libfoo/1.0.0
stat libfoo/1.0.0 "fetched"
stat libfoo "fetched 1.0.0"

# multiple versions/revisions
#
test cfg-create --wipe
test rep-add ../tests/repository/1/status/extra
test rep-fetch
stat libbar "available 1.1.0-1"
test rep-add ../tests/repository/1/status/stable
test rep-fetch
stat libbar "available 1.1.0-1 1.0.0"

test cfg-create --wipe
test rep-add ../tests/repository/1/status/testing
test rep-fetch
stat libbar "available 1.1.0 1.0.0-1 1.0.0"

test cfg-create --wipe
test rep-add ../tests/repository/1/status/unstable
test rep-fetch
stat libbar "available 2.0.0 1.1.0 1.0.0-1 1.0.0"
test pkg-fetch libbar/1.0.0-1
stat libbar "fetched 1.0.0-1; available 2.0.0 1.1.0"
test pkg-purge libbar
test pkg-fetch libbar/2.0.0
stat libbar "fetched 2.0.0"

##
## pkg-update
##

fail pkg-update           # package name expected
fail pkg-update $pkg      # no such package
test pkg-fetch -e $pkga
fail pkg-update $pkg      # wrong package state
test pkg-purge $pkg

# src == out
#
test pkg-fetch -e $pkga
test pkg-unpack $pkg
test pkg-configure $pkg
test pkg-update $pkg
test pkg-update $pkg
test pkg-disfigure $pkg
test pkg-purge $pkg

# src != out
#
test pkg-unpack -e $pkgd
test pkg-configure $pkg
test pkg-update $pkg
test pkg-update $pkg
test pkg-disfigure $pkg
test pkg-purge $pkg

##
## pkg-clean
##

fail pkg-clean           # package name expected
fail pkg-clean $pkg      # no such package
test pkg-fetch -e $pkga
fail pkg-clean $pkg      # wrong package state
test pkg-purge $pkg

# src == out
#
test pkg-fetch -e $pkga
test pkg-unpack $pkg
test pkg-configure $pkg
test pkg-update $pkg
test pkg-clean $pkg
test pkg-clean $pkg
test pkg-disfigure $pkg
test pkg-purge $pkg

# src != out
#
test pkg-unpack -e $pkgd
test pkg-configure $pkg
test pkg-update $pkg
test pkg-clean $pkg
test pkg-clean $pkg
test pkg-disfigure $pkg
test pkg-purge $pkg

##
## Low-level command scenarios.
##

# build package from remote repository
#
test cfg-create --wipe cxx
test rep-add http://pkg.cppget.org/1/hello
test rep-fetch
test pkg-fetch $pkg/$ver
test pkg-unpack $pkg
test pkg-configure $pkg
test pkg-update $pkg
test pkg-clean $pkg
test pkg-disfigure $pkg
test pkg-purge $pkg

##
## High-level commands.
##

##
## build
##

# 1
#
test rep-create ../tests/repository/1/satisfy/t1
test cfg-create --wipe

fail build -p               # package name expected
fail build -p libfoo        # unknown package
fail build -p libfoo/1.0.0  # unknown package
test build -p ../tests/repository/1/satisfy/libfoo-1.1.0.tar.gz <<EOF
build libfoo 1.1.0
EOF
test build -p ../tests/repository/1/satisfy/libfoo-1.1.0 <<EOF
build libfoo 1.1.0
EOF

test pkg-unpack -e ../tests/repository/1/satisfy/libfoo-1.1.0
test build -p libfoo <<< "build libfoo 1.1.0"
test build -p libfoo/1.1.0 <<< "build libfoo 1.1.0"
test build -p libfoo libfoo <<< "build libfoo 1.1.0"
test build -p libfoo libfoo/1.1.0 <<< "build libfoo 1.1.0"
test build -p libfoo/1.1.0 libfoo <<< "build libfoo 1.1.0"
test build -p libfoo/1.1.0 libfoo/1.1.0 <<< "build libfoo 1.1.0"
fail build -p libfoo/1.0.0
test pkg-purge libfoo

test rep-add ../tests/repository/1/satisfy/t1
test rep-fetch
test build -p libfoo <<< "build libfoo 1.0.0"
test build -p libfoo/1.0.0 <<< "build libfoo 1.0.0"
test build -p libfoo libfoo <<< "build libfoo 1.0.0"
test build -p libfoo libfoo/1.0.0 <<< "build libfoo 1.0.0"
test build -p libfoo/1.0.0 libfoo <<< "build libfoo 1.0.0"
test build -p libfoo/1.0.0 libfoo/1.0.0 <<< "build libfoo 1.0.0"
fail build -p libfoo/1.1.0

test pkg-unpack -e ../tests/repository/1/satisfy/libfoo-1.1.0
test build -p libfoo <<< "build libfoo 1.1.0"
test build -p libfoo/1.0.0 <<< "downgrade libfoo 1.0.0"
fail build -p libfoo/0.0.0
test pkg-purge libfoo

test pkg-fetch -e ../tests/repository/1/satisfy/libfoo-0.0.0.tar.gz
test pkg-unpack libfoo
test build -p libfoo <<< "upgrade libfoo 1.0.0"
test build -p libfoo/0.0.0 <<< "build libfoo 0.0.0"
fail build -p libfoo/1.1.0
test pkg-purge libfoo

# 2 (libbar depends on libfoo)
#
test rep-create ../tests/repository/1/satisfy/t2
test cfg-create --wipe

fail build ../tests/repository/1/satisfy/libbar-1.0.0.tar.gz

test rep-add ../tests/repository/1/satisfy/t2
test rep-fetch

test build -p libbar <<EOF
build libfoo 1.0.0
build libbar 1.0.0
EOF
test build -p libbar libfoo <<EOF
build libfoo 1.0.0
build libbar 1.0.0
EOF
test build -p libbar libfoo/1.0.0 <<EOF
build libfoo 1.0.0
build libbar 1.0.0
EOF
test build -p libbar libfoo libbar/1.0.0 <<EOF
build libfoo 1.0.0
build libbar 1.0.0
EOF
fail build -p libbar libfoo/1.1.0

test pkg-fetch -e ../tests/repository/1/satisfy/libfoo-0.0.0.tar.gz
test pkg-unpack libfoo
test build -p libbar <<EOF
build libfoo 0.0.0
build libbar 1.0.0
EOF
test build -p libbar libfoo <<EOF
upgrade libfoo 1.0.0
build libbar 1.0.0
EOF
test build -p libbar libfoo/0.0.0 <<EOF
build libfoo 0.0.0
build libbar 1.0.0
EOF
test pkg-purge libfoo

test pkg-unpack -e ../tests/repository/1/satisfy/libfoo-1.1.0
test build -p libbar <<EOF
build libfoo 1.1.0
build libbar 1.0.0
EOF
test build -p libbar libfoo <<EOF
build libfoo 1.1.0
build libbar 1.0.0
EOF
test build -p libbar libfoo/1.0.0 <<EOF
downgrade libfoo 1.0.0
build libbar 1.0.0
EOF
test pkg-purge libfoo

# 3 (libbaz depends on libbar; libbar in prerequisite repository)
#
test rep-create ../tests/repository/1/satisfy/t3
test cfg-create --wipe
test rep-add ../tests/repository/1/satisfy/t3
test rep-fetch

# only in prerequisite repository
#
fail build -p libfoo
fail build -p libbar
fail build -p libbaz libbar

test build -p libbaz <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test rep-add ../tests/repository/1/satisfy/t2
test rep-fetch

# order
#
test build -p libfox libfoo <<EOF
build libfox 1.0.0
build libfoo 1.0.0
EOF

test build -p libfoo libfox <<EOF
build libfoo 1.0.0
build libfox 1.0.0
EOF

test build -p libbaz libfoo <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test build -p libfoo libbaz <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test build -p libbaz libfox <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
build libfox 1.0.0
EOF

test build -p libfox libbaz <<EOF
build libfox 1.0.0
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test build -p libfox libfoo libbaz <<EOF
build libfox 1.0.0
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test build -p libfox libbaz libfoo <<EOF
build libfox 1.0.0
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test build -p libfoo libfox libbaz <<EOF
build libfoo 1.0.0
build libfox 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test build -p libfoo libbaz libfox <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
build libfox 1.0.0
EOF

# this one is contradictory: baz before fox but fox before foo
#
test build -p libbaz libfox libfoo <<EOF
build libfox 1.0.0
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test build -p libbaz libfoo libfox <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
build libfox 1.0.0
EOF

test build -p libbaz libfoo libbar <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test build -p libbaz libbar libfoo <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

# 4 (libbaz depends on libfoo and libbar; libbar depends on libfoo >= 1.1.0)
#
test rep-create ../tests/repository/1/satisfy/t4a
test rep-create ../tests/repository/1/satisfy/t4b
test rep-create ../tests/repository/1/satisfy/t4c
test cfg-create --wipe
test rep-add ../tests/repository/1/satisfy/t4c
test rep-fetch

test build -p libbaz <<EOF
build libfoo 1.1.0
build libbar 1.1.0
build libbaz 1.1.0
EOF

test build -p libfoo libbaz <<EOF
build libfoo 1.1.0
build libbar 1.1.0
build libbaz 1.1.0
EOF

fail build -p libfoo/1.0.0 libbaz
fail build -p libfoo/1.1.0 libbaz

# upgrade warning
#
test pkg-fetch -e ../tests/repository/1/satisfy/libfoo-0.0.0.tar.gz
test pkg-unpack libfoo
test build -p libbaz <<EOF
upgrade libfoo 1.1.0
build libbar 1.1.0
build libbaz 1.1.0
EOF
test pkg-purge libfoo

# downgrade error
#
test pkg-fetch -e ../tests/repository/1/satisfy/libfoo-1.2.0.tar.gz
test pkg-unpack libfoo
fail build -p libbaz
test rep-add ../tests/repository/1/satisfy/t4a
test rep-fetch
test build -p libfoo/1.1.0 libbaz <<EOF
downgrade libfoo 1.1.0
build libbar 1.1.0
build libbaz 1.1.0
EOF
test pkg-purge libfoo

# dependent prevents upgrade/downgrade
#
test pkg-fetch libfoo/1.1.0
test pkg-unpack libfoo
test pkg-configure libfoo
test pkg-fetch libbar/1.1.0
test pkg-unpack libbar
test pkg-configure libbar
fail build -p ../tests/repository/1/satisfy/libfoo-1.2.0.tar.gz
fail build -p libfoo/1.0.0
test build -p libfoo/1.1.0 <<< "build libfoo 1.1.0"
test pkg-disfigure libbar
test pkg-disfigure libfoo
test pkg-purge libbar
test pkg-purge libfoo

# dependent reconfigure
#
test cfg-create --wipe

test pkg-fetch -e ../tests/repository/1/satisfy/libfoo-1.0.0.tar.gz
test pkg-unpack libfoo
test pkg-configure libfoo
test pkg-fetch -e ../tests/repository/1/satisfy/libbar-1.0.0.tar.gz
test pkg-unpack libbar
test pkg-configure libbar
test pkg-fetch -e ../tests/repository/1/satisfy/libbaz-1.1.0.tar.gz
test pkg-unpack libbaz
test pkg-configure libbaz

test rep-add ../tests/repository/1/satisfy/t4a
test rep-add ../tests/repository/1/satisfy/t4b
test rep-fetch

test build -p libbar <<EOF
upgrade libfoo 1.1.0
upgrade libbar 1.1.0
reconfigure libbaz
EOF

test build -p libfoo <<EOF
upgrade libfoo 1.1.0
reconfigure libbar
reconfigure libbaz
EOF

test build -p libfoo libbar/1.0.0 <<EOF
upgrade libfoo 1.1.0
reconfigure/build libbar 1.0.0
reconfigure libbaz
EOF

test build -p libbar/1.0.0 libfoo <<EOF
upgrade libfoo 1.1.0
reconfigure/build libbar 1.0.0
reconfigure libbaz
EOF

test build -p libbaz libfoo <<EOF
upgrade libfoo 1.1.0
reconfigure libbar
reconfigure/build libbaz 1.1.0
EOF

test build -p libbaz libfoo/1.0.0 <<EOF
build libfoo 1.0.0
build libbaz 1.1.0
EOF
