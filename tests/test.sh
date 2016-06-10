#! /usr/bin/env bash

# Usage: test.sh [test-options] [bpkg-common-options]
#
# Test options are:
#
# -v
#    Run verbose. By default, this script runs bpkg quiet and suppresses
#    error messages in the fail tests. Note that when this options is
#    specified, bpkg is called with default verbosity level. If you want
#    more bpkg diagnostics, add the --verbose N option.
#
# --remote
#    Test using the remote repositories. Normally, you would first run the
#    local test in order to create the repositories, then publish them (see
#    pkg/publish.sh), and finally run the remote test.
#
# --valgrind
#    Run under valgrind (takes forever).
#
# Some common bpkg use-cases worth testing:
#
# --fetch wget
# --fetch curl
# --fetch fetch --fetch-option --no-verify-peer
#
# --tar bsdtar
#
# --sha256 shasum
# --sha256 sha256-freebsd
#

trap 'exit 1' ERR

tmp_file=`mktemp`

# Remove temporary file on exit. Cover the case when exit due to an error.
#
trap 'rm -f $tmp_file' EXIT

function error ()
{
  echo "$*" 1>&2
  exit 1
}

bpkg="../bpkg/bpkg"
cfg=/tmp/conf

if [ "${MSYSTEM:0:5}" = "MINGW" -o "${MSYSTEM:0:4}" = "MSYS" ]; then
  msys=y
fi

verbose=n
remote=n
options=

while [ $# -gt 0 ]; do
  case $1 in
    -v)
      verbose=y
      shift
      ;;
    --remote)
      remote=y
      shift
      ;;
    --valgrind)
      bpkg="valgrind -q $bpkg"
      shift
      ;;
    *)
      # If this is the --verbose bpkg option, switch to the verbose
      # mode as well.
      #
      if [ "$1" == "--verbose" ]; then
        verbose=y
      fi

      options="$options $1"
      shift
      ;;
  esac
done

if [ "$verbose" != "y" ]; then
  options="$options -q"
fi

bpkg="$bpkg $options"

# Repository location and name prefixes. Note that the local path is carefully
# crafted so that we end up with the same repository names in both cases. This
# is necessary for the authentication tests to work in both cases.
#
if [ "$remote" = "y" ]; then
  rep=https://build2.org/bpkg/1
  repn=build2.org/
else
  rep=pkg/1/build2.org
  repn=build2.org/
fi

#
#
function test ()
{
  local cmd=$1; shift
  local ops=

  if [ "$cmd" != "rep-create" -a \
       "$cmd" != "rep-info" -a   \
       "$cmd" != "pkg-verify" ]; then
    ops="-d $cfg"
  fi

  if [ "$cmd" = "cfg-fetch" -o \
       "$cmd" = "rep-info" ]; then
    ops="$ops --auth all"
  fi

  if [ -t 0 ]; then
    $bpkg $cmd $ops $*
  else
    $bpkg $cmd $ops $* >$tmp_file
    diff --strip-trailing-cr -u - $tmp_file
  fi

  if [ $? -ne 0 ]; then
    error "failed: $bpkg $cmd $ops $*"
  fi
}

function fail ()
{
  local cmd=$1; shift
  local ops=

  if [ "$cmd" != "rep-create" -a \
       "$cmd" != "rep-info" -a   \
       "$cmd" != "pkg-verify" ]; then
    ops="-d $cfg"
  fi

  if [ "$cmd" = "cfg-fetch" -o \
       "$cmd" = "rep-info" ]; then
    ops="$ops --auth all"
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
  local s=`$bpkg pkg-status -d $cfg $1`

  if [ "$s" != "$2" ]; then
    error "status $1: '"$s"', expected: '"$2"'"
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

#if false; then

# Repository certificate fingerprint.
#
function rep_cert_fp ()
{
  cat $1/repositories | \
  sed -n '/^-----BEGIN CERTIFICATE-----$/,/^-----END CERTIFICATE-----$/p' | \
  openssl x509 -sha256 -noout -fingerprint | \
  sed -n 's/^SHA256 Fingerprint=\(.*\)$/\1/p'
}

# Edit file with sed.
#
function edit ()
{
  local path=$1; shift
  sed "$@" $path > $path.bak
  mv $path.bak $path
}

# Repository absolute location from a relative path.
#
function location ()
{
  if [ "$remote" = "y" ]; then
    echo $rep/$1
  elif [ "$msys" = "y" ]; then
    # Convert Windows path like c:/abc/xyz to the c:\abc\xyz canonical form.
    #
    echo `pwd -W`/$rep/$1 | sed 's%/%\\%g'
  else
    echo `pwd`/$rep/$1
  fi
}

##
## Low-level commands.
##


##
## pkg-verify
##
fail pkg-verify                 # archive expected
fail pkg-verify ./no-such-file  # archive does not exist
fail pkg-verify pkg/1/build2.org/common/not-a-package.tar.gz
fail pkg-verify --silent pkg/1/build2.org/common/not-a-package.tar.gz
test pkg-verify pkg/1/build2.org/common/hello/libhello-1.0.0+1.tar.gz


##
## rep-create
##
fail rep-create                          # no 'repositories' file
fail rep-create pkg/1/build2.org/satisfy # unexpected files

test rep-create pkg/1/build2.org/common/hello --key key.pem

test rep-create pkg/1/build2.org/common/foo/stable
test rep-create pkg/1/build2.org/common/foo/testing

test rep-create pkg/1/build2.org/common/bar/stable
test rep-create pkg/1/build2.org/common/bar/testing
test rep-create pkg/1/build2.org/common/bar/unstable


##
## rep-info
##
fail rep-info # repository location expected

test rep-info --trust-yes $rep/common/foo/testing <<EOF
${repn}common/foo/testing `location common/foo/testing`
complement ${repn}common/foo/stable `location common/foo/stable`
libfoo 1.1.0
EOF

test rep-info -m -r -n --trust-yes $rep/common/bar/unstable <<EOF
${repn}common/bar/unstable `location common/bar/unstable`
: 1
location: ../../foo/testing
:
location: ../testing
role: complement
:
EOF

test rep-info -m -p --trust-yes $rep/common/bar/unstable <<EOF
: 1
sha256sum: 3034b727288efbb52b7b6e41fe147b815e7b3aa704e8cef6c2ee8d7421ab5b72
:
name: libbar
version: 1.1.1
summary: libbar
license: MIT
description: \\
libbar is a very modern C++ XML parser.

It has an API that we believe should have already been in Boost or even in
the C++ standard library.

\\
changes: \\
* Applied upstream patch for critical bug bar.

* Applied upstream patch for critical bug foo.

\\
url: http://example.org
email: pkg@example.org
depends: libfoo >= 1.1.0
location: libbar-1.1.1.tar.gz
sha256sum: d09700602ff78ae405b6d4850e34660e939d27676e015a23b549884497c8bb45
EOF

hello_fp=`rep_cert_fp pkg/1/build2.org/common/hello`
test rep-info -m -p --trust $hello_fp $rep/common/hello <<EOF
: 1
sha256sum: 8d324fa7911038778b215d28805c6546e737e0092f79f7bd167cf2e28f4ad96f
:
name: libhello
version: 1.0.0+1
summary: The "Hello World" example library
license: MIT
tags: c++, hello, world, example
description: \\
A simple library that implements the "Hello World" example in C++. Its primary
goal is to show a canonical build2/bpkg project/package.
\\
url: http://www.example.org/libhello
email: hello-users@example.org
requires: c++11
location: libhello-1.0.0+1.tar.gz
sha256sum: ceff9f39dbff496ece817d6806ab3723b065dcdff1734683fe64a60c103f7f9b
EOF

##
## cfg-create
##
test cfg-create --wipe cxx config.install.root=/tmp/install
stat libfoo unknown

test cfg-create --wipe config.install.root=/tmp/install cxx
stat libfoo unknown


##
## cfg-add
##
test cfg-create --wipe

fail cfg-add         # repository location expected
fail cfg-add stable  # invalid location
fail cfg-add http:// # invalid location

# relative path
#
test cfg-add ./1/bar/stable
fail cfg-add ./1/../1/bar/stable # duplicate

# absolute path
#
test cfg-add /tmp/1/foo/stable
fail cfg-add /tmp/1/../1/foo/stable # duplicate

# remote URL
#
test cfg-add http://pkg.example.org/1/testing
fail cfg-add https://www.example.org/1/testing # duplicate


##
## cfg-fetch
##
test cfg-create --wipe

fail cfg-fetch # no repositories

# hello repository
#
test cfg-create --wipe
test cfg-add $rep/common/hello
test cfg-fetch --trust $hello_fp
test cfg-fetch

# bar/unstable repository
#
test cfg-create --wipe
test cfg-add $rep/common/bar/unstable
test cfg-fetch --trust-yes
test cfg-fetch

# both
#
test cfg-create --wipe
test cfg-add $rep/common/hello
test cfg-add $rep/common/bar/unstable
test cfg-fetch --trust-yes
test cfg-fetch


##
## pkg-fetch
##
test rep-create pkg/1/build2.org/fetch/t1
test cfg-create --wipe

fail pkg-fetch -e                # archive expected
fail pkg-fetch -e ./no-such-file # archive does not exist

fail pkg-fetch                   # package name expected
fail pkg-fetch libfoo            # package version expected
fail pkg-fetch libfoo/1/2/3      # invalid package version

fail pkg-fetch libfoo/1.0.0      # no repositories
test cfg-add $rep/fetch/t1
fail pkg-fetch libfoo/1.0.0      # no packages
test cfg-fetch --trust-yes
fail pkg-fetch libfoo/2+1.0.0    # not available
test cfg-create --wipe
test cfg-add $rep/fetch/t1
test cfg-fetch --trust-yes
test pkg-fetch libfoo/1.0.0
stat libfoo/1.0.0 fetched
fail pkg-fetch libfoo/1.0.0
fail pkg-fetch -e pkg/1/build2.org/fetch/libfoo-1.0.0.tar.gz
test pkg-purge libfoo
test pkg-fetch -e pkg/1/build2.org/fetch/libfoo-1.0.0.tar.gz
stat libfoo/1.0.0 fetched
test pkg-unpack libfoo
test pkg-fetch -r libfoo/1.1.0
stat libfoo/1.1.0 fetched
test pkg-unpack libfoo
test pkg-fetch -r -e pkg/1/build2.org/fetch/libfoo-1.0.0.tar.gz
stat libfoo/1.0.0 fetched
test pkg-fetch -r libfoo/1.1.0
stat libfoo/1.1.0 fetched
test pkg-fetch -r -e pkg/1/build2.org/fetch/libfoo-1.0.0.tar.gz
stat libfoo/1.0.0 fetched
test pkg-purge libfoo

# hello
#
test cfg-create --wipe
test cfg-add $rep/common/hello
test cfg-fetch --trust $hello_fp
test pkg-fetch libhello/1.0.0+1
test pkg-purge libhello


##
## pkg-unpack
##
test cfg-create --wipe
fail pkg-unpack -r # replace only with existing
fail pkg-unpack -e # package directory expected
fail pkg-unpack    # package name expected

test cfg-add $rep/fetch/t1
test cfg-fetch --trust-yes

# existing
#
fail pkg-unpack -e ./no-such-dir # package directory does not exist
fail pkg-unpack -e ./pkg         # not a package directory
test pkg-fetch libfoo/1.0.0
fail pkg-unpack -e pkg/1/build2.org/fetch/libfoo-1.1.0 # already exists
test pkg-purge libfoo
test pkg-unpack -e pkg/1/build2.org/fetch/libfoo-1.1.0
stat libfoo/1.1.0 unpacked
test pkg-purge libfoo

# existing & replace
#
test pkg-fetch libfoo/1.0.0
fail pkg-unpack -e pkg/1/build2.org/fetch/libfoo-1.1.0
test pkg-unpack -r -e pkg/1/build2.org/fetch/libfoo-1.1.0
stat libfoo/1.1.0 unpacked
test pkg-purge libfoo
test pkg-fetch libfoo/1.0.0
test pkg-unpack libfoo
fail pkg-unpack -e pkg/1/build2.org/fetch/libfoo-1.1.0
test pkg-unpack -r -e pkg/1/build2.org/fetch/libfoo-1.1.0
stat libfoo/1.1.0 unpacked
test pkg-purge libfoo

# package name
#
fail pkg-unpack libfoo # no such package in configuration
test pkg-unpack -e pkg/1/build2.org/fetch/libfoo-1.1.0
fail pkg-unpack libfoo # wrong package state
test pkg-purge libfoo
test pkg-fetch libfoo/1.0.0
stat libfoo/1.0.0 fetched
test pkg-unpack libfoo
stat libfoo/1.0.0 unpacked
test pkg-purge libfoo

# hello
#
test cfg-create --wipe
test cfg-add $rep/common/hello
test cfg-fetch --trust $hello_fp
test pkg-fetch libhello/1.0.0+1
test pkg-unpack libhello
test pkg-purge libhello


##
## pkg-purge
##
test cfg-create --wipe

fail pkg-purge         # missing package name
fail pkg-purge libfoo  # no such package

# purge fetched
#
test pkg-fetch -e pkg/1/build2.org/fetch/libfoo-1.0.0.tar.gz
test pkg-purge libfoo
stat libfoo unknown

# --keep
#
test pkg-fetch -e pkg/1/build2.org/fetch/libfoo-1.0.0.tar.gz
test pkg-purge -k libfoo
stat libfoo "fetched 1.0.0"
test pkg-purge libfoo

# archive and --purge
#
cp pkg/1/build2.org/fetch/libfoo-1.0.0.tar.gz $cfg/
test pkg-fetch -e -p $cfg/libfoo-1.0.0.tar.gz
test pkg-purge libfoo
stat libfoo unknown
gone $cfg/libfoo-1.0.0.tar.gz

# no archive but --keep
#
test pkg-unpack -e pkg/1/build2.org/fetch/libfoo-1.1.0
fail pkg-purge --keep libfoo
stat libfoo "unpacked 1.1.0"
test pkg-purge libfoo

# purge unpacked directory
#
test pkg-unpack -e pkg/1/build2.org/fetch/libfoo-1.1.0
test pkg-purge libfoo
stat libfoo unknown

# purge unpacked archive
#
test pkg-fetch -e pkg/1/build2.org/fetch/libfoo-1.0.0.tar.gz
test pkg-unpack libfoo
test pkg-purge libfoo
stat libfoo unknown
gone $cfg/libfoo-1.0.0

# purge unpacked archive but --keep
#
test pkg-fetch -e pkg/1/build2.org/fetch/libfoo-1.0.0.tar.gz
test pkg-unpack libfoo
test pkg-purge --keep libfoo
stat libfoo "fetched 1.0.0"
gone $cfg/libfoo-1.0.0
test pkg-purge libfoo
stat libfoo unknown

# directory and --purge
#
cp -r pkg/1/build2.org/fetch/libfoo-1.1.0 $cfg/
test pkg-unpack -e -p $cfg/libfoo-1.1.0
test pkg-purge libfoo
stat libfoo unknown
gone $cfg/libfoo-1.1.0

# archive and --purge
#
cp pkg/1/build2.org/fetch/libfoo-1.0.0.tar.gz $cfg/
test pkg-fetch -e -p $cfg/libfoo-1.0.0.tar.gz
test pkg-unpack libfoo
test pkg-purge libfoo
stat libfoo unknown
gone $cfg/libfoo-1.0.0
gone $cfg/libfoo-1.0.0.tar.gz

# broken
#
cp pkg/1/build2.org/fetch/libfoo-1.0.0.tar.gz $cfg/
test pkg-fetch -e -p $cfg/libfoo-1.0.0.tar.gz
test pkg-unpack libfoo
chmod 000 $cfg/libfoo-1.0.0
fail pkg-purge libfoo
stat libfoo/1.0.0 broken
fail pkg-purge libfoo        # need --force
fail pkg-purge -f -k libfoo  # can't keep broken
fail pkg-purge -f libfoo     # out directory still exists
chmod 755 $cfg/libfoo-1.0.0
rm -r $cfg/libfoo-1.0.0
fail pkg-purge -f libfoo     # archive still exists
rm $cfg/libfoo-1.0.0.tar.gz
test pkg-purge -f libfoo
stat libfoo unknown


##
## pkg-configure/pkg-disfigure
##
test cfg-create --wipe
test cfg-add $rep/common/hello
test cfg-fetch --trust $hello_fp

fail pkg-configure                        # package name expected
fail pkg-configure config.dist.root=/tmp  # ditto
fail pkg-configure libhello libhello      # unexpected argument
fail pkg-configure libhello1              # no such package

fail pkg-disfigure                        # package name expected
fail pkg-disfigure libhello1              # no such package

test pkg-fetch libhello/1.0.0+1

fail pkg-configure libhello               # wrong package state
fail pkg-disfigure libhello               # wrong package state

test pkg-purge libhello

# src == out
#
test pkg-fetch libhello/1.0.0+1
test pkg-unpack libhello
test pkg-configure libhello
stat libhello "configured 1.0.0+1"
test pkg-disfigure libhello
stat libhello "unpacked 1.0.0+1"
test pkg-purge libhello
stat libhello/1.0.0 "available 1.0.0+1"

# src != out
#
test cfg-create --wipe
test pkg-unpack -e pkg/1/build2.org/common/libhello-1.0.0+1
test pkg-configure libhello
stat libhello "configured 1.0.0+1"
test pkg-disfigure libhello
stat libhello "unpacked 1.0.0+1"
test pkg-purge libhello
stat libhello unknown
gone $cfg/libhello-1.0.0+1

# out still exists after disfigure
#
test pkg-unpack -e pkg/1/build2.org/common/libhello-1.0.0+1
test pkg-configure libhello
touch $cfg/libhello-1.0.0+1/stray
fail pkg-disfigure libhello
stat libhello/1.0.0+1 broken
rm -r $cfg/libhello-1.0.0+1
test pkg-purge -f libhello
stat libhello unknown

# disfigure failed
#
test pkg-unpack -e pkg/1/build2.org/common/libhello-1.0.0+1
test pkg-configure libhello
chmod 555 $cfg/libhello-1.0.0+1
fail pkg-disfigure libhello
stat libhello/1.0.0+1 broken
chmod 755 $cfg/libhello-1.0.0+1
rm -r $cfg/libhello-1.0.0+1
test pkg-purge -f libhello
stat libhello unknown

# While it's forbidden to delete a directory with write permissions being
# revoked with the 'chmod 555' command in MSYS, it's still allowed to create
# subdirectories and files inside such a directory. This is why the following
# 'fail pkg-configure libhello' test cases undesirably succeed in MSYS.
#
if [ "$msys" != "y" ]; then
  # configure failed but disfigure succeeds
  #
  test pkg-unpack -e pkg/1/build2.org/common/libhello-1.0.0+1
  mkdir -p $cfg/libhello-1.0.0+1/build
  chmod 555 $cfg/libhello-1.0.0+1/build
  fail pkg-configure libhello
  stat libhello "unpacked 1.0.0+1"
  test pkg-purge libhello
  stat libhello unknown

  # configure and disfigure both failed
  #
  test pkg-unpack -e pkg/1/build2.org/common/libhello-1.0.0+1
  mkdir -p $cfg/libhello-1.0.0+1/build
  # Trip both con/dis.
  #
  chmod 555 $cfg/libhello-1.0.0+1 $cfg/libhello-1.0.0+1/build
  fail pkg-configure libhello
  stat libhello/1.0.0+1 broken
  chmod 755 $cfg/libhello-1.0.0+1 $cfg/libhello-1.0.0+1/build
  rm -r $cfg/libhello-1.0.0+1
  test pkg-purge -f libhello
  stat libhello unknown
fi

# dependency management
#
test rep-create pkg/1/build2.org/depend/stable
test cfg-create --wipe
test cfg-add $rep/depend/stable
test cfg-fetch --trust-yes

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
## pkg-status (also tested in pkg-{fetch,unpack,configure,disfigure,purge})
##
test rep-create pkg/1/build2.org/status/stable
test rep-create pkg/1/build2.org/status/extra
test rep-create pkg/1/build2.org/status/testing
test rep-create pkg/1/build2.org/status/unstable

# basics
#
test cfg-create --wipe
stat libfoo/1.0.0 "unknown"
stat libfoo "unknown"
test cfg-add $rep/status/stable
test cfg-fetch --trust-yes
stat libfoo/1.0.0 "available"
stat libfoo "available 1.0.0"
test pkg-fetch libfoo/1.0.0
stat libfoo/1.0.0 "fetched"
stat libfoo "fetched 1.0.0"

# multiple versions/revisions
#
test cfg-create --wipe
test cfg-add $rep/status/extra
test cfg-fetch --trust-yes
stat libbar "available 1.1.0+1"
test cfg-add $rep/status/stable
test cfg-fetch --trust-yes
stat libbar "available 1.1.0+1 1.0.0"

test cfg-create --wipe
test cfg-add $rep/status/testing
test cfg-fetch --trust-yes
stat libbar "available 1.1.0 1.0.0+1 1.0.0"

test cfg-create --wipe
test cfg-add $rep/status/unstable
test cfg-fetch --trust-yes
stat libbar "available 2.0.0 1.1.0 1.0.0+1 1.0.0"
test pkg-fetch libbar/1.0.0+1
stat libbar "fetched 1.0.0+1; available 2.0.0 1.1.0"
test pkg-purge libbar
test pkg-fetch libbar/2.0.0
stat libbar "fetched 2.0.0"


##
## pkg-update
##
test cfg-create --wipe
test cfg-add $rep/common/hello
test cfg-fetch --trust $hello_fp

fail pkg-update                # package name expected
fail pkg-update libhello       # no such package
test pkg-fetch libhello/1.0.0+1
fail pkg-update libhello       # wrong package state
test pkg-purge libhello

# src == out
#
test pkg-fetch libhello/1.0.0+1
test pkg-unpack libhello
test pkg-configure libhello
test pkg-update libhello
test pkg-update libhello
test pkg-disfigure libhello
test pkg-purge libhello

# src != out
#
test cfg-create --wipe
test pkg-unpack -e pkg/1/build2.org/common/libhello-1.0.0+1
test pkg-configure libhello
test pkg-update libhello
test pkg-update libhello
test pkg-disfigure libhello
test pkg-purge libhello


##
## pkg-clean
##
test cfg-create --wipe
test cfg-add $rep/common/hello
test cfg-fetch --trust $hello_fp

fail pkg-clean                  # package name expected
fail pkg-clean libhello         # no such package
test pkg-fetch libhello/1.0.0+1
fail pkg-clean libhello         # wrong package state
test pkg-purge libhello

# src == out
#
test pkg-fetch libhello/1.0.0+1
test pkg-unpack libhello
test pkg-configure libhello
test pkg-update libhello
test pkg-clean libhello
test pkg-clean libhello
test pkg-disfigure libhello
test pkg-purge libhello

# src != out
#
test cfg-create --wipe
test pkg-unpack -e pkg/1/build2.org/common/libhello-1.0.0+1
test pkg-configure libhello
test pkg-update libhello
test pkg-clean libhello
test pkg-clean libhello
test pkg-disfigure libhello
test pkg-purge libhello


##
## Low-level command scenarios.
##


# build and clean package
#
test cfg-create --wipe cxx
test cfg-add $rep/common/hello
test cfg-fetch --trust $hello_fp
test pkg-fetch libhello/1.0.0+1
test pkg-unpack libhello
test pkg-configure libhello
test pkg-update libhello
test pkg-clean libhello
test pkg-disfigure libhello
test pkg-purge libhello

##
## pkg-build
##

# 1 (libfoo)
#
test rep-create pkg/1/build2.org/satisfy/t1
test cfg-create --wipe

fail pkg-build -p               # package name expected
fail pkg-build -p libfoo        # unknown package
fail pkg-build -p libfoo/1.0.0  # unknown package
test pkg-build -p pkg/1/build2.org/satisfy/libfoo-1.1.0.tar.gz <<EOF
build libfoo 1.1.0
EOF
test pkg-build -p pkg/1/build2.org/satisfy/libfoo-1.1.0/ <<EOF
build libfoo 1.1.0
EOF

test pkg-unpack -e pkg/1/build2.org/satisfy/libfoo-1.1.0
test pkg-build -p libfoo <<< "build libfoo 1.1.0"
test pkg-build -p libfoo/1.1.0 <<< "build libfoo 1.1.0"
test pkg-build -p libfoo libfoo <<< "build libfoo 1.1.0"
test pkg-build -p libfoo libfoo/1.1.0 <<< "build libfoo 1.1.0"
test pkg-build -p libfoo/1.1.0 libfoo <<< "build libfoo 1.1.0"
test pkg-build -p libfoo/1.1.0 libfoo/1.1.0 <<< "build libfoo 1.1.0"
fail pkg-build -p libfoo/1.0.0
test pkg-purge libfoo

test cfg-add $rep/satisfy/t1
test cfg-fetch --trust-yes
test pkg-build -p libfoo <<< "build libfoo 1.0.0"
test pkg-build -p libfoo/1.0.0 <<< "build libfoo 1.0.0"
test pkg-build -p libfoo libfoo <<< "build libfoo 1.0.0"
test pkg-build -p libfoo libfoo/1.0.0 <<< "build libfoo 1.0.0"
test pkg-build -p libfoo/1.0.0 libfoo <<< "build libfoo 1.0.0"
test pkg-build -p libfoo/1.0.0 libfoo/1.0.0 <<< "build libfoo 1.0.0"
fail pkg-build -p libfoo/1.1.0

test pkg-unpack -e pkg/1/build2.org/satisfy/libfoo-1.1.0
test pkg-build -p libfoo <<< "build libfoo 1.1.0"
test pkg-build -p libfoo/1.0.0 <<< "downgrade libfoo 1.0.0"
fail pkg-build -p libfoo/0.0.0
test pkg-purge libfoo

test pkg-fetch -e pkg/1/build2.org/satisfy/libfoo-0.0.0.tar.gz
test pkg-unpack libfoo
test pkg-build -p libfoo <<< "upgrade libfoo 1.0.0"
test pkg-build -p libfoo/0.0.0 <<< "build libfoo 0.0.0"
fail pkg-build -p libfoo/1.1.0
test pkg-purge libfoo

# 2 (libbar depends on libfoo)
#
test rep-create pkg/1/build2.org/satisfy/t2
test cfg-create --wipe

fail pkg-build pkg/1/build2.org/satisfy/libbar-1.0.0.tar.gz

test cfg-add $rep/satisfy/t2
test cfg-fetch --trust-yes

test pkg-build -p libbar <<EOF
build libfoo 1.0.0 (required by libbar)
build libbar 1.0.0
EOF
test pkg-build -p libbar libfoo <<EOF
build libfoo 1.0.0
build libbar 1.0.0
EOF
test pkg-build -p libbar libfoo/1.0.0 <<EOF
build libfoo 1.0.0
build libbar 1.0.0
EOF
test pkg-build -p libbar libfoo libbar/1.0.0 <<EOF
build libfoo 1.0.0
build libbar 1.0.0
EOF
fail pkg-build -p libbar libfoo/1.1.0

test pkg-fetch -e pkg/1/build2.org/satisfy/libfoo-0.0.0.tar.gz
test pkg-unpack libfoo
test pkg-build -p libbar <<EOF
build libfoo 0.0.0 (required by libbar)
build libbar 1.0.0
EOF
test pkg-build -p libbar libfoo <<EOF
upgrade libfoo 1.0.0
build libbar 1.0.0
EOF
test pkg-build -p libbar libfoo/0.0.0 <<EOF
build libfoo 0.0.0
build libbar 1.0.0
EOF
test pkg-purge libfoo

test pkg-unpack -e pkg/1/build2.org/satisfy/libfoo-1.1.0
test pkg-build -p libbar <<EOF
build libfoo 1.1.0 (required by libbar)
build libbar 1.0.0
EOF
test pkg-build -p libbar libfoo <<EOF
build libfoo 1.1.0
build libbar 1.0.0
EOF
test pkg-build -p libbar libfoo/1.0.0 <<EOF
downgrade libfoo 1.0.0
build libbar 1.0.0
EOF
test pkg-purge libfoo

# 3 (libbaz depends on libbar; libbar in prerequisite repository)
#
test rep-create pkg/1/build2.org/satisfy/t3
test cfg-create --wipe
test cfg-add $rep/satisfy/t3
test cfg-fetch --trust-yes

# only in prerequisite repository
#
fail pkg-build -p libfoo
fail pkg-build -p libbar
fail pkg-build -p libbaz libbar

test pkg-build -p libbaz <<EOF
build libfoo 1.0.0 (required by libbar)
build libbar 1.0.0 (required by libbaz)
build libbaz 1.0.0
EOF

test cfg-add $rep/satisfy/t2
test cfg-fetch

# order
#
test pkg-build -p libfox libfoo <<EOF
build libfox 1.0.0
build libfoo 1.0.0
EOF

test pkg-build -p libfoo libfox <<EOF
build libfoo 1.0.0
build libfox 1.0.0
EOF

test pkg-build -p libbaz libfoo <<EOF
build libfoo 1.0.0
build libbar 1.0.0 (required by libbaz)
build libbaz 1.0.0
EOF

test pkg-build -p libfoo libbaz <<EOF
build libfoo 1.0.0
build libbar 1.0.0 (required by libbaz)
build libbaz 1.0.0
EOF

test pkg-build -p libbaz libfox <<EOF
build libfoo 1.0.0 (required by libbar)
build libbar 1.0.0 (required by libbaz)
build libbaz 1.0.0
build libfox 1.0.0
EOF

test pkg-build -p libfox libbaz <<EOF
build libfox 1.0.0
build libfoo 1.0.0 (required by libbar)
build libbar 1.0.0 (required by libbaz)
build libbaz 1.0.0
EOF

test pkg-build -p libfox libfoo libbaz <<EOF
build libfox 1.0.0
build libfoo 1.0.0
build libbar 1.0.0 (required by libbaz)
build libbaz 1.0.0
EOF

test pkg-build -p libfox libbaz libfoo <<EOF
build libfox 1.0.0
build libfoo 1.0.0
build libbar 1.0.0 (required by libbaz)
build libbaz 1.0.0
EOF

test pkg-build -p libfoo libfox libbaz <<EOF
build libfoo 1.0.0
build libfox 1.0.0
build libbar 1.0.0 (required by libbaz)
build libbaz 1.0.0
EOF

test pkg-build -p libfoo libbaz libfox <<EOF
build libfoo 1.0.0
build libbar 1.0.0 (required by libbaz)
build libbaz 1.0.0
build libfox 1.0.0
EOF

# this one is contradictory: baz before fox but fox before foo
#
test pkg-build -p libbaz libfox libfoo <<EOF
build libfox 1.0.0
build libfoo 1.0.0
build libbar 1.0.0 (required by libbaz)
build libbaz 1.0.0
EOF

test pkg-build -p libbaz libfoo libfox <<EOF
build libfoo 1.0.0
build libbar 1.0.0 (required by libbaz)
build libbaz 1.0.0
build libfox 1.0.0
EOF

test pkg-build -p libbaz libfoo libbar <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test pkg-build -p libbaz libbar libfoo <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

# 4 (libbaz depends on libfoo and libbar; libbar depends on libfoo >= 1.1.0)
#
test rep-create pkg/1/build2.org/satisfy/t4a
test rep-create pkg/1/build2.org/satisfy/t4b
test rep-create pkg/1/build2.org/satisfy/t4c
test rep-create pkg/1/build2.org/satisfy/t4d

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch --trust-yes

test pkg-build -p libbaz <<EOF
build libfoo 1.1.0 (required by libbar libbaz)
build libbar 1.1.0 (required by libbaz)
build libbaz 1.1.0
EOF

test pkg-build -p libfoo libbaz <<EOF
build libfoo 1.1.0
build libbar 1.1.0 (required by libbaz)
build libbaz 1.1.0
EOF

fail pkg-build -p libfoo/1.0.0 libbaz
fail pkg-build -p libfoo/1.1.0 libbaz

# upgrade warning
#
test pkg-fetch -e pkg/1/build2.org/satisfy/libfoo-0.0.0.tar.gz
test pkg-unpack libfoo
test pkg-build -p libbaz <<EOF
upgrade libfoo 1.1.0 (required by libbar libbaz)
build libbar 1.1.0 (required by libbaz)
build libbaz 1.1.0
EOF
test pkg-purge libfoo

# downgrade error
#
test pkg-fetch -e pkg/1/build2.org/satisfy/libfoo-1.2.0.tar.gz
test pkg-unpack libfoo
fail pkg-build -p libbaz
test cfg-add $rep/satisfy/t4a
test cfg-fetch --trust-yes
test pkg-build -p libfoo/1.1.0 libbaz <<EOF
downgrade libfoo 1.1.0
build libbar 1.1.0 (required by libbaz)
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
fail pkg-build -p pkg/1/build2.org/satisfy/libfoo-1.2.0.tar.gz
fail pkg-build -p libfoo/1.0.0
test pkg-build -p libfoo/1.1.0 <<< "build libfoo 1.1.0"
test pkg-disfigure libbar
test pkg-disfigure libfoo
test pkg-purge libbar
test pkg-purge libfoo

# dependent reconfigure
#
test cfg-create --wipe

test pkg-fetch -e pkg/1/build2.org/satisfy/libfoo-1.0.0.tar.gz
test pkg-unpack libfoo
test pkg-configure libfoo
test pkg-fetch -e pkg/1/build2.org/satisfy/libbar-1.0.0.tar.gz
test pkg-unpack libbar
test pkg-configure libbar
test pkg-fetch -e pkg/1/build2.org/satisfy/libbaz-1.1.0.tar.gz
test pkg-unpack libbaz
test pkg-configure libbaz

test cfg-add $rep/satisfy/t4a
test cfg-add $rep/satisfy/t4b
test cfg-fetch --trust-yes

test pkg-build -p libbar <<EOF
upgrade libfoo 1.1.0 (required by libbar libbaz)
upgrade libbar 1.1.0
reconfigure libbaz (required by libbar)
EOF

test pkg-build -p libfoo <<EOF
upgrade libfoo 1.1.0
reconfigure libbar (required by libfoo)
reconfigure libbaz (required by libbar)
EOF

test pkg-build -p libfoo libbar/1.0.0 <<EOF
upgrade libfoo 1.1.0
reconfigure/build libbar 1.0.0
reconfigure libbaz (required by libbar)
EOF

test pkg-build -p libbar/1.0.0 libfoo <<EOF
upgrade libfoo 1.1.0
reconfigure/build libbar 1.0.0
reconfigure libbaz (required by libbar)
EOF

test pkg-build -p libbaz libfoo <<EOF
upgrade libfoo 1.1.0
reconfigure libbar (required by libbaz libfoo)
reconfigure/build libbaz 1.1.0
EOF

test pkg-build -p libbaz libfoo/1.0.0 <<EOF
build libfoo 1.0.0
build libbaz 1.1.0
EOF

# actually build
#
test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch --trust-yes
test pkg-build -y libbaz
stat libfoo/1.1.0 "configured"
stat libbar/1.1.0 "configured"
stat libbaz/1.1.0 "configured hold_package"

# hold
#
test cfg-create --wipe
test pkg-build -y pkg/1/build2.org/satisfy/libfoo-1.0.0.tar.gz
stat libfoo "configured 1.0.0 hold_package hold_version"
test pkg-build -y pkg/1/build2.org/satisfy/libfoo-1.1.0/
stat libfoo "configured 1.1.0 hold_package hold_version"

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch --trust-yes
test pkg-build -y libfoo
stat libfoo "configured 1.0.0 hold_package"
test pkg-build -y libfoo/1.0.0
stat libfoo "configured 1.0.0 hold_package hold_version"

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch --trust-yes
test pkg-build -y libfoo/1.0.0
stat libfoo "configured 1.0.0 hold_package hold_version"

test cfg-create --wipe
test pkg-fetch -e pkg/1/build2.org/satisfy/libfoo-1.0.0.tar.gz
test pkg-unpack libfoo
test pkg-configure libfoo
stat libfoo "configured 1.0.0"
test pkg-build -y libfoo
stat libfoo "configured 1.0.0 hold_package"

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch --trust-yes
test pkg-build -y libfoo
stat libfoo "configured 1.0.0 hold_package"
test pkg-build -y libbaz
stat libfoo "configured 1.1.0 hold_package"

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch --trust-yes
test pkg-build -y libfoo/1.0.0
stat libfoo "configured 1.0.0 hold_package hold_version"
fail pkg-build -y libbaz

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch --trust-yes
test pkg-build -y libbaz
stat libfoo "configured 1.1.0"

# drop prerequisites on downgrade
#
test rep-create pkg/1/build2.org/satisfy/t5
test cfg-create --wipe
test cfg-add $rep/satisfy/t2
test cfg-fetch --trust-yes

test pkg-build -y libbar
stat libfoo "configured 1.0.0"
stat libbar "configured 1.0.0 hold_package"

test cfg-add $rep/satisfy/t5
test cfg-fetch --trust-yes

test pkg-build -y libbar
stat libfoo "available 1.0.0"
stat libbar "configured 1.2.0 hold_package"

test pkg-build -y libbar/1.0.0 libfoo
stat libfoo "configured 1.0.0 hold_package"
stat libbar "configured 1.0.0 hold_package hold_version; available 1.2.0"

test pkg-build -y libbar
stat libfoo "configured 1.0.0 hold_package"
stat libbar "configured 1.2.0 hold_package"

##
## pkg-drop
##
test cfg-create --wipe

fail pkg-drop -p               # package name expected
fail pkg-drop -p libfoo        # unknown package
fail pkg-drop -p libfoo/1.0.0  # unknown package

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch --trust-yes
test pkg-build -y libbaz

test pkg-drop -p -y libfoo libbaz libbar <<EOF
drop libbaz
drop libbar
drop libfoo
EOF

# dependents
#
fail pkg-drop -y libfoo
fail pkg-drop -y libfoo libbar
fail pkg-drop -y libfoo libbaz

test pkg-drop -p -y --drop-dependent libfoo <<EOF
drop libbaz
drop libbar
drop libfoo
EOF

test pkg-drop -p --drop-dependent libfoo libbaz <<EOF
drop libbaz
drop libbar
drop libfoo
EOF

test pkg-drop -p --drop-dependent libbaz libfoo <<EOF
drop libbaz
drop libbar
drop libfoo
EOF

# prerequisites
#
test pkg-drop -p -y libbaz <<EOF
drop libbaz
drop libbar
drop libfoo
EOF

test pkg-drop -p -n libbaz <<EOF
drop libbaz
EOF

test pkg-drop -p -n libbar libbaz <<EOF
drop libbaz
drop libbar
EOF

test pkg-drop -p -n libbaz libbar <<EOF
drop libbaz
drop libbar
EOF

# prerequisites and dependents
#
test pkg-drop -p -y --drop-dependent libbar <<EOF
drop libbaz
drop libbar
drop libfoo
EOF

test cfg-create --wipe
test cfg-add $rep/satisfy/t4d
test cfg-fetch --trust-yes
test pkg-build -y libbiz

test pkg-drop -p -y libbiz <<EOF
drop libbiz
drop libbaz
drop libbar
drop libfoo
drop libfox
EOF

test pkg-drop -p -y libfox libbiz <<EOF
drop libbiz
drop libfox
drop libbaz
drop libbar
drop libfoo
EOF

test pkg-drop -p -y --drop-dependent libfox <<EOF
drop libbiz
drop libfox
drop libbaz
drop libbar
drop libfoo
EOF

test pkg-drop -p -y --drop-dependent libbaz <<EOF
drop libbiz
drop libbaz
drop libbar
drop libfoo
drop libfox
EOF

test pkg-drop -p -y --drop-dependent libbar <<EOF
drop libbiz
drop libbaz
drop libbar
drop libfoo
drop libfox
EOF

test pkg-drop -p -y --drop-dependent libfoo <<EOF
drop libbiz
drop libbaz
drop libbar
drop libfoo
drop libfox
EOF

test pkg-drop -p -n --drop-dependent libfox libbaz <<EOF
drop libbiz
drop libfox
drop libbaz
EOF

test pkg-drop -p -n --drop-dependent libbaz libfox <<EOF
drop libbiz
drop libbaz
drop libfox
EOF

test pkg-drop -p -n --drop-dependent libfox libbar <<EOF
drop libbiz
drop libfox
drop libbaz
drop libbar
EOF

test pkg-drop -p -n --drop-dependent libbar libfox <<EOF
drop libbiz
drop libbaz
drop libbar
drop libfox
EOF

# actually drop
#
test pkg-drop -y --drop-dependent libbar
stat libfox/1.0.0 "available"
stat libfoo/1.1.0 "unknown"
stat libbar/1.1.0 "unknown"
stat libbaz/1.1.0 "unknown"
stat libbiz/1.0.0 "available"

##
## auth
##

# rep-create
#
fail rep-create pkg/1/build2.org/auth/create-noemail --key key.pem
fail rep-create pkg/1/build2.org/auth/create-expired --key key.pem

fail rep-create pkg/1/build2.org/auth/signed # no --key option
test rep-create pkg/1/build2.org/auth/signed --key key.pem
test rep-create pkg/1/build2.org/auth/unsigned1
test rep-create pkg/1/build2.org/auth/unsigned2
test rep-create pkg/1/build2.org/auth/name-mismatch --key key.pem

test rep-create pkg/1/build2.org/auth/sha256sum-mismatch --key key.pem

# Tamper signature manifest's sha256sum value.
#
s=d374c59b36fdbdbd0d4468665061d94fda9c6c687863dfe72b0bcc34ff9d5fb4
edit pkg/1/build2.org/auth/sha256sum-mismatch/signature \
     "s/^\(sha256sum: \).*\$/\1$s/"

test rep-create pkg/1/build2.org/auth/signature-mismatch --key key.pem

# Tamper signature manifest's signature value.
#
edit pkg/1/build2.org/auth/signature-mismatch/signature \
     '/^signature: \\$/,/^\\$/d'
cat >> pkg/1/build2.org/auth/signature-mismatch/signature << EOF
signature: \\
XBjnmXXVHY0RqMI0gL/P4t/vuWwK9JJkLl4Qf2gMxq5k2WQ2CIE56DfG0RaGklgKcI3UxsQZvMQI
5PNtAHJDjteQ+BqY0io8A43KPX+2LKMU+I825sKmPRjCLYleGM3mNndDkWfYtAzYk5AmR2piqRz0
D7CLq9GIoQQZO4Fw44muaQDMCRcXy8Txx2jDnretQjx/C0ZQw4M/cd6/cKEKUmLITDkBig9oVlSh
tpxHqWz5NTbO3vm8ILc03AwiOJHwZweLb6ocJ6a467IJa+F/xUm9B09k8wFWMs+jHXXzHDE0syv7
lqWL7SvHSjVFrGVFKS6nx7lCj2b8XFiGlwWIwjY4m/VK/5QmbL/lC4f+ww5XT5NG5iYh/eMaCxCJ
zTg5iZsWNLhrx9uKNrL5xC4z0OONRVOwzu7gsqr0GLWewPyhH0AqJLgOSkw9N7FJwbv2IKNZ88YA
u2YMXNkXytcQvENLVQDX5oxvUMEurUJFOCuYB/SEnpcwkV5h9RtXzIFVy4OCTU2MhQHDEldI8s7w
Hga/ct4WupgE228gGdgwJLCbHx6AWBlS9iL10AdS8JkQ9LaZwTMHHz44f8y00X4MiT06gpgDeoQD
rUyP0KNG65tdWnVTMqg6Q/YXhtRZLHoD6+QbiYLlruR1phu4y4fDt7AKxoXfeme/a86A37UogZY=
\\
EOF

# cfg-fetch
#
signed_fp=`rep_cert_fp pkg/1/build2.org/auth/signed`
test cfg-create --wipe
test cfg-add $rep/auth/signed
test cfg-fetch --trust $signed_fp
test cfg-fetch

test cfg-create --wipe
test cfg-add $rep/auth/signed
test cfg-fetch --trust-no --trust $signed_fp
test cfg-fetch
test cfg-fetch --trust-no # certificate is already trusted

test cfg-create --wipe
test cfg-add $rep/auth/signed
test cfg-fetch --trust-yes
test cfg-fetch

test cfg-create --wipe
test cfg-add $rep/auth/signed
fail cfg-fetch --trust-no

test cfg-create --wipe
test cfg-add $rep/auth/signed
fail cfg-fetch --trust-yes --trust-no # inconsistent options

test cfg-create --wipe
test cfg-add $rep/auth/unsigned1
test cfg-fetch --trust-yes
test cfg-fetch
test cfg-add $rep/auth/unsigned2
test cfg-fetch
test cfg-fetch --trust-no # certificates are already trusted

test cfg-create --wipe
test cfg-add $rep/auth/unsigned1
fail cfg-fetch --trust-no

test cfg-create --wipe
test cfg-add $rep/auth/name-mismatch
fail cfg-fetch --trust-yes # certificate name mismatch

test cfg-create --wipe
test cfg-add $rep/auth/expired
fail cfg-fetch --trust-yes # certificate expired

test cfg-create --wipe
test cfg-add $rep/auth/sha256sum-mismatch
fail cfg-fetch --trust-yes # packages file checksum mismatch

test cfg-create --wipe
test cfg-add $rep/auth/signature-mismatch
fail cfg-fetch --trust-yes # packages file signature:mismatch

# rep-info
#
test cfg-create --wipe
test rep-info --trust-no --trust $signed_fp -d $cfg $rep/auth/signed <<EOF
${repn}auth/signed `location auth/signed`
libfoo 1.0.0
EOF

test rep-info --trust-no -d $cfg $rep/auth/signed <<EOF
${repn}auth/signed `location auth/signed`
libfoo 1.0.0
EOF

test cfg-create --wipe
test rep-info --trust-yes $rep/auth/signed <<EOF
${repn}auth/signed `location auth/signed`
libfoo 1.0.0
EOF

fail rep-info --trust-no $rep/auth/signed <<EOF
${repn}auth/signed `location auth/signed`
libfoo 1.0.0
EOF

test cfg-create --wipe
test rep-info --trust-yes -d $cfg $rep/auth/unsigned1 <<EOF
${repn}auth/unsigned1 `location auth/unsigned1`
libfoo 1.0.0
EOF

test rep-info --trust-no -d $cfg $rep/auth/unsigned2 <<EOF
${repn}auth/unsigned2 `location auth/unsigned2`
libfoo 1.0.0
EOF

test cfg-create --wipe
test rep-info --trust-yes $rep/auth/unsigned1 <<EOF
${repn}auth/unsigned1 `location auth/unsigned1`
libfoo 1.0.0
EOF

fail rep-info --trust-no $rep/auth/unsigned1 <<EOF
${repn}auth/unsigned1 `location auth/unsigned1`
libfoo 1.0.0
EOF
