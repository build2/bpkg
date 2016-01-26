#! /usr/bin/env bash

version="0.2.0"
date="XX January 2016"

trap 'exit 1' ERR
set -o errtrace # Trap in functions.

function info () { echo "$*" 1>&2; }
function error () { info "$*"; exit 1; }

while [ $# -gt 0 ]; do
  case $1 in
    --clean)
      rm -f bpkg*.xhtml bpkg*.1
      exit 0
      ;;
    *)
      error "unexpected $1"
      ;;
  esac
done

function compile ()
{
  local n=$1; shift

  # Use a bash array to handle empty arguments.
  #
  local o=()
  while [ $# -gt 0 ]; do
    o=("${o[@]}" "$1")
    shift
  done

  cli -I .. -v version="$version" -v date="$date" --include-base-last "${o[@]}" \
--generate-html --html-prologue-file prologue.xhtml --html-epilogue-file \
epilogue.xhtml --html-suffix .xhtml ../bpkg/$n.cli

  cli -I .. -v version="$version" -v date="$date" --include-base-last "${o[@]}" \
--generate-man --man-prologue-file prologue.1 --man-epilogue-file epilogue.1 \
--man-suffix .1 ../bpkg/$n.cli
}

o="--output-prefix bpkg- --class-doc bpkg::common_options=short"

# A few special cases.
#
compile "common" $o --output-suffix "-options" --class-doc bpkg::common_options=long
compile "bpkg" $o --output-prefix "" --suppress-undocumented --class-doc bpkg::commands=short --class-doc bpkg::topics=short

pages="cfg-add cfg-create cfg-fetch help pkg-build pkg-clean pkg-configure \
pkg-disfigure pkg-drop pkg-fetch pkg-install pkg-purge pkg-status \
pkg-test pkg-uninstall pkg-unpack pkg-update pkg-verify rep-create rep-info"

for p in $pages; do
  compile $p $o
done
