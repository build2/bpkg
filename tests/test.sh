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
#    repository/publish.sh), and finally run the remote test.
#
# --valgrind
#    Run under valgrind (takes forever).
#
# Some common bpkg use-cases worth testing:
#
# --fetch wget
# --fetch curl
# --fetch fetch --tar bsdtar
#

trap 'exit 1' ERR

function error ()
{
  echo "$*" 1>&2
  exit 1
}

bpkg="../bpkg/bpkg"
cfg=/tmp/conf

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

# Repository location, name, and absolute location prefixes.
#
if [ "$remote" = "y" ]; then
  rep=http://pkg.cppget.org/tests/1
  repn=cppget.org/
  repa=$rep
else
  rep=repository/1
  repn=
  repa=`pwd`/$rep
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

  if [ "$cmd" != "rep-create" -a \
       "$cmd" != "rep-info" -a   \
       "$cmd" != "pkg-verify" ]; then
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


##
## Low-level commands.
##


##
## pkg-verify
##
fail pkg-verify                 # archive expected
fail pkg-verify ./no-such-file  # archive does not exist
fail pkg-verify repository/1/common/not-a-package.tar.gz
fail pkg-verify --silent repository/1/common/not-a-package.tar.gz
test pkg-verify repository/1/common/hello/libhello-1.0.0.tar.gz


##
## rep-create
##
fail rep-create                      # no 'repositories' file
fail rep-create repository/1/satisfy # unexpected files

test rep-create repository/1/common/hello

test rep-create repository/1/common/foo/stable
test rep-create repository/1/common/foo/testing

test rep-create repository/1/common/bar/stable
test rep-create repository/1/common/bar/testing
test rep-create repository/1/common/bar/unstable


##
## rep-info
##
fail rep-info # repository location expected

test rep-info $rep/common/foo/testing <<EOF
${repn}common/foo/testing $repa/common/foo/testing
complement ${repn}common/foo/stable $repa/common/foo/stable
libfoo 1.1.0
EOF

test rep-info -m -r -n $rep/common/bar/unstable <<EOF
${repn}common/bar/unstable $repa/common/bar/unstable
: 1
location: ../../foo/testing
:
location: ../testing
role: complement
:
EOF

test rep-info -m -p $rep/common/bar/unstable <<EOF
: 1
name: libbar
version: 1.1.1
summary: libbar
license: MIT
url: http://example.org
email: pkg@example.org
depends: libfoo >= 1.1.0
location: libbar-1.1.1.tar.gz
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
fail cfg-add http://www.example.org/1/testing # duplicate


##
## cfg-fetch
##
test cfg-create --wipe

fail cfg-fetch # no repositories

# hello repository
#
test cfg-create --wipe
test cfg-add $rep/common/hello
test cfg-fetch
test cfg-fetch

# bar/unstable repository
#
test cfg-create --wipe
test cfg-add $rep/common/bar/unstable
test cfg-fetch
test cfg-fetch

# both
#
test cfg-create --wipe
test cfg-add $rep/common/hello
test cfg-add $rep/common/bar/unstable
test cfg-fetch
test cfg-fetch


##
## pkg-fetch
##
test rep-create repository/1/fetch/t1
test cfg-create --wipe

fail pkg-fetch -e                # archive expected
fail pkg-fetch -e ./no-such-file # archive does not exist

fail pkg-fetch                   # package name expected
fail pkg-fetch libfoo            # package version expected
fail pkg-fetch libfoo/1/2/3      # invalid package version

fail pkg-fetch libfoo/1.0.0      # no repositories
test cfg-add $rep/fetch/t1
fail pkg-fetch libfoo/1.0.0      # no packages
test cfg-fetch
fail pkg-fetch libfoo/2+1.0.0    # not available

test cfg-create --wipe
test cfg-add $rep/fetch/t1
test cfg-fetch
test pkg-fetch libfoo/1.0.0
stat libfoo/1.0.0 fetched
fail pkg-fetch libfoo/1.0.0
fail pkg-fetch -e repository/1/fetch/libfoo-1.0.0.tar.gz
test pkg-purge libfoo
test pkg-fetch -e repository/1/fetch/libfoo-1.0.0.tar.gz
stat libfoo/1.0.0 fetched
test pkg-unpack libfoo
test pkg-fetch -r libfoo/1.1.0
stat libfoo/1.1.0 fetched
test pkg-unpack libfoo
test pkg-fetch -r -e repository/1/fetch/libfoo-1.0.0.tar.gz
stat libfoo/1.0.0 fetched
test pkg-fetch -r libfoo/1.1.0
stat libfoo/1.1.0 fetched
test pkg-fetch -r -e repository/1/fetch/libfoo-1.0.0.tar.gz
stat libfoo/1.0.0 fetched
test pkg-purge libfoo

# hello
#
test cfg-create --wipe
test cfg-add $rep/common/hello
test cfg-fetch
test pkg-fetch libhello/1.0.0
test pkg-purge libhello


##
## pkg-unpack
##
test cfg-create --wipe
fail pkg-unpack -r # replace only with existing
fail pkg-unpack -e # package directory expected
fail pkg-unpack    # package name expected

test cfg-add $rep/fetch/t1
test cfg-fetch

# existing
#
fail pkg-unpack -e ./no-such-dir # package directory does not exist
fail pkg-unpack -e ./repository  # not a package directory
test pkg-fetch libfoo/1.0.0
fail pkg-unpack -e repository/1/fetch/libfoo-1.1.0 # already exists
test pkg-purge libfoo
test pkg-unpack -e repository/1/fetch/libfoo-1.1.0
stat libfoo/1.1.0 unpacked
test pkg-purge libfoo

# existing & replace
#
test pkg-fetch libfoo/1.0.0
fail pkg-unpack -e repository/1/fetch/libfoo-1.1.0
test pkg-unpack -r -e repository/1/fetch/libfoo-1.1.0
stat libfoo/1.1.0 unpacked
test pkg-purge libfoo
test pkg-fetch libfoo/1.0.0
test pkg-unpack libfoo
fail pkg-unpack -e repository/1/fetch/libfoo-1.1.0
test pkg-unpack -r -e repository/1/fetch/libfoo-1.1.0
stat libfoo/1.1.0 unpacked
test pkg-purge libfoo

# package name
#
fail pkg-unpack libfoo # no such package in configuration
test pkg-unpack -e repository/1/fetch/libfoo-1.1.0
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
test cfg-fetch
test pkg-fetch libhello/1.0.0
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
test pkg-fetch -e repository/1/fetch/libfoo-1.0.0.tar.gz
test pkg-purge libfoo
stat libfoo unknown

# --keep
#
test pkg-fetch -e repository/1/fetch/libfoo-1.0.0.tar.gz
test pkg-purge -k libfoo
stat libfoo "fetched 1.0.0"
test pkg-purge libfoo

# archive and --purge
#
cp repository/1/fetch/libfoo-1.0.0.tar.gz $cfg/
test pkg-fetch -e -p $cfg/libfoo-1.0.0.tar.gz
test pkg-purge libfoo
stat libfoo unknown
gone $cfg/libfoo-1.0.0.tar.gz

# no archive but --keep
#
test pkg-unpack -e repository/1/fetch/libfoo-1.1.0
fail pkg-purge --keep libfoo
stat libfoo "unpacked 1.1.0"
test pkg-purge libfoo

# purge unpacked directory
#
test pkg-unpack -e repository/1/fetch/libfoo-1.1.0
test pkg-purge libfoo
stat libfoo unknown

# purge unpacked archive
#
test pkg-fetch -e repository/1/fetch/libfoo-1.0.0.tar.gz
test pkg-unpack libfoo
test pkg-purge libfoo
stat libfoo unknown
gone $cfg/libfoo-1.0.0

# purge unpacked archive but --keep
#
test pkg-fetch -e repository/1/fetch/libfoo-1.0.0.tar.gz
test pkg-unpack libfoo
test pkg-purge --keep libfoo
stat libfoo "fetched 1.0.0"
gone $cfg/libfoo-1.0.0
test pkg-purge libfoo
stat libfoo unknown

# directory and --purge
#
cp -r repository/1/fetch/libfoo-1.1.0 $cfg/
test pkg-unpack -e -p $cfg/libfoo-1.1.0
test pkg-purge libfoo
stat libfoo unknown
gone $cfg/libfoo-1.1.0

# archive and --purge
#
cp repository/1/fetch/libfoo-1.0.0.tar.gz $cfg/
test pkg-fetch -e -p $cfg/libfoo-1.0.0.tar.gz
test pkg-unpack libfoo
test pkg-purge libfoo
stat libfoo unknown
gone $cfg/libfoo-1.0.0
gone $cfg/libfoo-1.0.0.tar.gz

# broken
#
cp repository/1/fetch/libfoo-1.0.0.tar.gz $cfg/
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
test cfg-fetch

fail pkg-configure                        # package name expected
fail pkg-configure config.dist.root=/tmp  # ditto
fail pkg-configure libhello libhello      # unexpected argument
fail pkg-configure libhello1              # no such package

fail pkg-disfigure                        # package name expected
fail pkg-disfigure libhello1              # no such package

test pkg-fetch libhello/1.0.0

fail pkg-configure libhello               # wrong package state
fail pkg-disfigure libhello               # wrong package state

test pkg-purge libhello

# src == out
#
test pkg-fetch libhello/1.0.0
test pkg-unpack libhello
test pkg-configure libhello
stat libhello "configured 1.0.0"
test pkg-disfigure libhello
stat libhello "unpacked 1.0.0"
test pkg-purge libhello
stat libhello/1.0.0 available

# src != out
#
test cfg-create --wipe
test pkg-unpack -e repository/1/common/libhello-1.0.0
test pkg-configure libhello
stat libhello "configured 1.0.0"
test pkg-disfigure libhello
stat libhello "unpacked 1.0.0"
test pkg-purge libhello
stat libhello unknown
gone $cfg/libhello-1.0.0

# out still exists after disfigure
#
test pkg-unpack -e repository/1/common/libhello-1.0.0
test pkg-configure libhello
touch $cfg/libhello-1.0.0/stray
fail pkg-disfigure libhello
stat libhello/1.0.0 broken
rm -r $cfg/libhello-1.0.0
test pkg-purge -f libhello
stat libhello unknown

# disfigure failed
#
test pkg-unpack -e repository/1/common/libhello-1.0.0
test pkg-configure libhello
chmod 555 $cfg/libhello-1.0.0
fail pkg-disfigure libhello
stat libhello/1.0.0 broken
chmod 755 $cfg/libhello-1.0.0
rm -r $cfg/libhello-1.0.0
test pkg-purge -f libhello
stat libhello unknown

# configure failed but disfigure succeeds
#
test pkg-unpack -e repository/1/common/libhello-1.0.0
mkdir -p $cfg/libhello-1.0.0/build
chmod 555 $cfg/libhello-1.0.0/build
fail pkg-configure libhello
stat libhello "unpacked 1.0.0"
test pkg-purge libhello
stat libhello unknown

# configure and disfigure both failed
#
test pkg-unpack -e repository/1/common/libhello-1.0.0
mkdir -p $cfg/libhello-1.0.0/build
chmod 555 $cfg/libhello-1.0.0 $cfg/libhello-1.0.0/build # Trip both con/dis.
fail pkg-configure libhello
stat libhello/1.0.0 broken
chmod 755 $cfg/libhello-1.0.0 $cfg/libhello-1.0.0/build
rm -r $cfg/libhello-1.0.0
test pkg-purge -f libhello
stat libhello unknown

# dependency management
#
test rep-create repository/1/depend/stable
test cfg-create --wipe
test cfg-add $rep/depend/stable
test cfg-fetch

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
test rep-create repository/1/status/stable
test rep-create repository/1/status/extra
test rep-create repository/1/status/testing
test rep-create repository/1/status/unstable

# basics
#
test cfg-create --wipe
stat libfoo/1.0.0 "unknown"
stat libfoo "unknown"
test cfg-add $rep/status/stable
test cfg-fetch
stat libfoo/1.0.0 "available"
stat libfoo "available 1.0.0"
test pkg-fetch libfoo/1.0.0
stat libfoo/1.0.0 "fetched"
stat libfoo "fetched 1.0.0"

# multiple versions/revisions
#
test cfg-create --wipe
test cfg-add $rep/status/extra
test cfg-fetch
stat libbar "available 1.1.0-1"
test cfg-add $rep/status/stable
test cfg-fetch
stat libbar "available 1.1.0-1 1.0.0"

test cfg-create --wipe
test cfg-add $rep/status/testing
test cfg-fetch
stat libbar "available 1.1.0 1.0.0-1 1.0.0"

test cfg-create --wipe
test cfg-add $rep/status/unstable
test cfg-fetch
stat libbar "available 2.0.0 1.1.0 1.0.0-1 1.0.0"
test pkg-fetch libbar/1.0.0-1
stat libbar "fetched 1.0.0-1; available 2.0.0 1.1.0"
test pkg-purge libbar
test pkg-fetch libbar/2.0.0
stat libbar "fetched 2.0.0"


##
## pkg-update
##
test cfg-create --wipe
test cfg-add $rep/common/hello
test cfg-fetch

fail pkg-update                # package name expected
fail pkg-update libhello       # no such package
test pkg-fetch libhello/1.0.0
fail pkg-update libhello       # wrong package state
test pkg-purge libhello

# src == out
#
test pkg-fetch libhello/1.0.0
test pkg-unpack libhello
test pkg-configure libhello
test pkg-update libhello
test pkg-update libhello
test pkg-disfigure libhello
test pkg-purge libhello

# src != out
#
test cfg-create --wipe
test pkg-unpack -e repository/1/common/libhello-1.0.0
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
test cfg-fetch

fail pkg-clean                # package name expected
fail pkg-clean libhello       # no such package
test pkg-fetch libhello/1.0.0
fail pkg-clean libhello       # wrong package state
test pkg-purge libhello

# src == out
#
test pkg-fetch libhello/1.0.0
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
test pkg-unpack -e repository/1/common/libhello-1.0.0
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
test cfg-fetch
test pkg-fetch libhello/1.0.0
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
test rep-create repository/1/satisfy/t1
test cfg-create --wipe

fail pkg-build -p               # package name expected
fail pkg-build -p libfoo        # unknown package
fail pkg-build -p libfoo/1.0.0  # unknown package
test pkg-build -p repository/1/satisfy/libfoo-1.1.0.tar.gz <<EOF
build libfoo 1.1.0
EOF
test pkg-build -p repository/1/satisfy/libfoo-1.1.0 <<EOF
build libfoo 1.1.0
EOF

test pkg-unpack -e repository/1/satisfy/libfoo-1.1.0
test pkg-build -p libfoo <<< "build libfoo 1.1.0"
test pkg-build -p libfoo/1.1.0 <<< "build libfoo 1.1.0"
test pkg-build -p libfoo libfoo <<< "build libfoo 1.1.0"
test pkg-build -p libfoo libfoo/1.1.0 <<< "build libfoo 1.1.0"
test pkg-build -p libfoo/1.1.0 libfoo <<< "build libfoo 1.1.0"
test pkg-build -p libfoo/1.1.0 libfoo/1.1.0 <<< "build libfoo 1.1.0"
fail pkg-build -p libfoo/1.0.0
test pkg-purge libfoo

test cfg-add $rep/satisfy/t1
test cfg-fetch
test pkg-build -p libfoo <<< "build libfoo 1.0.0"
test pkg-build -p libfoo/1.0.0 <<< "build libfoo 1.0.0"
test pkg-build -p libfoo libfoo <<< "build libfoo 1.0.0"
test pkg-build -p libfoo libfoo/1.0.0 <<< "build libfoo 1.0.0"
test pkg-build -p libfoo/1.0.0 libfoo <<< "build libfoo 1.0.0"
test pkg-build -p libfoo/1.0.0 libfoo/1.0.0 <<< "build libfoo 1.0.0"
fail pkg-build -p libfoo/1.1.0

test pkg-unpack -e repository/1/satisfy/libfoo-1.1.0
test pkg-build -p libfoo <<< "build libfoo 1.1.0"
test pkg-build -p libfoo/1.0.0 <<< "downgrade libfoo 1.0.0"
fail pkg-build -p libfoo/0.0.0
test pkg-purge libfoo

test pkg-fetch -e repository/1/satisfy/libfoo-0.0.0.tar.gz
test pkg-unpack libfoo
test pkg-build -p libfoo <<< "upgrade libfoo 1.0.0"
test pkg-build -p libfoo/0.0.0 <<< "build libfoo 0.0.0"
fail pkg-build -p libfoo/1.1.0
test pkg-purge libfoo

# 2 (libbar depends on libfoo)
#
test rep-create repository/1/satisfy/t2
test cfg-create --wipe

fail pkg-build repository/1/satisfy/libbar-1.0.0.tar.gz

test cfg-add $rep/satisfy/t2
test cfg-fetch

test pkg-build -p libbar <<EOF
build libfoo 1.0.0
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

test pkg-fetch -e repository/1/satisfy/libfoo-0.0.0.tar.gz
test pkg-unpack libfoo
test pkg-build -p libbar <<EOF
build libfoo 0.0.0
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

test pkg-unpack -e repository/1/satisfy/libfoo-1.1.0
test pkg-build -p libbar <<EOF
build libfoo 1.1.0
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
test rep-create repository/1/satisfy/t3
test cfg-create --wipe
test cfg-add $rep/satisfy/t3
test cfg-fetch

# only in prerequisite repository
#
fail pkg-build -p libfoo
fail pkg-build -p libbar
fail pkg-build -p libbaz libbar

test pkg-build -p libbaz <<EOF
build libfoo 1.0.0
build libbar 1.0.0
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
build libbar 1.0.0
build libbaz 1.0.0
EOF

test pkg-build -p libfoo libbaz <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test pkg-build -p libbaz libfox <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
build libfox 1.0.0
EOF

test pkg-build -p libfox libbaz <<EOF
build libfox 1.0.0
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test pkg-build -p libfox libfoo libbaz <<EOF
build libfox 1.0.0
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test pkg-build -p libfox libbaz libfoo <<EOF
build libfox 1.0.0
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test pkg-build -p libfoo libfox libbaz <<EOF
build libfoo 1.0.0
build libfox 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test pkg-build -p libfoo libbaz libfox <<EOF
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
build libfox 1.0.0
EOF

# this one is contradictory: baz before fox but fox before foo
#
test pkg-build -p libbaz libfox libfoo <<EOF
build libfox 1.0.0
build libfoo 1.0.0
build libbar 1.0.0
build libbaz 1.0.0
EOF

test pkg-build -p libbaz libfoo libfox <<EOF
build libfoo 1.0.0
build libbar 1.0.0
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
test rep-create repository/1/satisfy/t4a
test rep-create repository/1/satisfy/t4b
test rep-create repository/1/satisfy/t4c
test rep-create repository/1/satisfy/t4d

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch

test pkg-build -p libbaz <<EOF
build libfoo 1.1.0
build libbar 1.1.0
build libbaz 1.1.0
EOF

test pkg-build -p libfoo libbaz <<EOF
build libfoo 1.1.0
build libbar 1.1.0
build libbaz 1.1.0
EOF

fail pkg-build -p libfoo/1.0.0 libbaz
fail pkg-build -p libfoo/1.1.0 libbaz

# upgrade warning
#
test pkg-fetch -e repository/1/satisfy/libfoo-0.0.0.tar.gz
test pkg-unpack libfoo
test pkg-build -p libbaz <<EOF
upgrade libfoo 1.1.0
build libbar 1.1.0
build libbaz 1.1.0
EOF
test pkg-purge libfoo

# downgrade error
#
test pkg-fetch -e repository/1/satisfy/libfoo-1.2.0.tar.gz
test pkg-unpack libfoo
fail pkg-build -p libbaz
test cfg-add $rep/satisfy/t4a
test cfg-fetch
test pkg-build -p libfoo/1.1.0 libbaz <<EOF
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
fail pkg-build -p repository/1/satisfy/libfoo-1.2.0.tar.gz
fail pkg-build -p libfoo/1.0.0
test pkg-build -p libfoo/1.1.0 <<< "build libfoo 1.1.0"
test pkg-disfigure libbar
test pkg-disfigure libfoo
test pkg-purge libbar
test pkg-purge libfoo

# dependent reconfigure
#
test cfg-create --wipe

test pkg-fetch -e repository/1/satisfy/libfoo-1.0.0.tar.gz
test pkg-unpack libfoo
test pkg-configure libfoo
test pkg-fetch -e repository/1/satisfy/libbar-1.0.0.tar.gz
test pkg-unpack libbar
test pkg-configure libbar
test pkg-fetch -e repository/1/satisfy/libbaz-1.1.0.tar.gz
test pkg-unpack libbaz
test pkg-configure libbaz

test cfg-add $rep/satisfy/t4a
test cfg-add $rep/satisfy/t4b
test cfg-fetch

test pkg-build -p libbar <<EOF
upgrade libfoo 1.1.0
upgrade libbar 1.1.0
reconfigure libbaz
EOF

test pkg-build -p libfoo <<EOF
upgrade libfoo 1.1.0
reconfigure libbar
reconfigure libbaz
EOF

test pkg-build -p libfoo libbar/1.0.0 <<EOF
upgrade libfoo 1.1.0
reconfigure/build libbar 1.0.0
reconfigure libbaz
EOF

test pkg-build -p libbar/1.0.0 libfoo <<EOF
upgrade libfoo 1.1.0
reconfigure/build libbar 1.0.0
reconfigure libbaz
EOF

test pkg-build -p libbaz libfoo <<EOF
upgrade libfoo 1.1.0
reconfigure libbar
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
test cfg-fetch
test pkg-build -y libbaz
stat libfoo/1.1.0 "configured"
stat libbar/1.1.0 "configured"
stat libbaz/1.1.0 "configured hold_package"

# hold
#
test cfg-create --wipe
test pkg-build -y repository/1/satisfy/libfoo-1.0.0.tar.gz
stat libfoo "configured 1.0.0 hold_package hold_version"
test pkg-build -y repository/1/satisfy/libfoo-1.1.0
stat libfoo "configured 1.1.0 hold_package hold_version"

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch
test pkg-build -y libfoo
stat libfoo "configured 1.0.0 hold_package"
test pkg-build -y libfoo/1.0.0
stat libfoo "configured 1.0.0 hold_package hold_version"

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch
test pkg-build -y libfoo/1.0.0
stat libfoo "configured 1.0.0 hold_package hold_version"

test cfg-create --wipe
test pkg-fetch -e repository/1/satisfy/libfoo-1.0.0.tar.gz
test pkg-unpack libfoo
test pkg-configure libfoo
stat libfoo "configured 1.0.0"
test pkg-build -y libfoo
stat libfoo "configured 1.0.0 hold_package"

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch
test pkg-build -y libfoo
stat libfoo "configured 1.0.0 hold_package"
test pkg-build -y libbaz
stat libfoo "configured 1.1.0 hold_package"

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch
test pkg-build -y libfoo/1.0.0
stat libfoo "configured 1.0.0 hold_package hold_version"
fail pkg-build -y libbaz

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch
test pkg-build -y libbaz
stat libfoo "configured 1.1.0"


##
## pkg-drop
##
test cfg-create --wipe

fail pkg-drop -p               # package name expected
fail pkg-drop -p libfoo        # unknown package
fail pkg-drop -p libfoo/1.0.0  # unknown package

test cfg-create --wipe
test cfg-add $rep/satisfy/t4c
test cfg-fetch
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
test cfg-fetch
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
