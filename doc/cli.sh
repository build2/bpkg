#! /usr/bin/env bash

version=0.17.0

trap 'exit 1' ERR
set -o errtrace # Trap in functions.

function info () { echo "$*" 1>&2; }
function error () { info "$*"; exit 1; }

date="$(date +"%B %Y")"
copyright="$(sed -n -re 's%^Copyright \(c\) (.+) \(see the AUTHORS and LEGAL files\)\.$%\1%p' ../LICENSE)"

while [ $# -gt 0 ]; do
  case $1 in
    --clean)
      rm -f bpkg*.xhtml bpkg*.1
      rm -f build2-package-manager-manual*.ps \
         build2-package-manager-manual*.pdf   \
         build2-package-manager-manual.xhtml
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

  cli -I .. \
-v project="bpkg" \
-v version="$version" \
-v date="$date" \
-v copyright="$copyright" \
--include-base-last "${o[@]}" \
--generate-html --html-suffix .xhtml \
--html-prologue-file man-prologue.xhtml \
--html-epilogue-file man-epilogue.xhtml \
--link-regex '%b([-.].+)%../../build2/doc/b$1%' \
--link-regex '%b(#.+)?%../../build2/doc/build2-build-system-manual.xhtml$1%' \
--link-regex '%bpkg(#.+)?%build2-package-manager-manual.xhtml$1%' \
../bpkg/$n.cli

  cli -I .. \
-v project="bpkg" \
-v version="$version" \
-v date="$date" \
-v copyright="$copyright" \
--include-base-last "${o[@]}" \
--generate-man --man-suffix .1 --ascii-tree \
--man-prologue-file man-prologue.1 \
--man-epilogue-file man-epilogue.1 \
--link-regex '%b(#.+)?%$1%' \
--link-regex '%bpkg(#.+)?%$1%' \
--link-regex '%#.+%%' \
../bpkg/$n.cli
}

# Need global --suppress-undocumented because of few undocumented options
# in common.cli.
#
o="--suppress-undocumented --output-prefix bpkg- --class-doc bpkg::common_options=short"

# A few special cases.
#
compile "common" $o --output-suffix "-options" --class-doc bpkg::common_options=long
compile "bpkg" $o --output-prefix "" --class-doc bpkg::commands=short --class-doc bpkg::topics=short

compile "pkg-build" $o --class-doc bpkg::pkg_build_pkg_options=exclude-base

compile "pkg-bindist" $o \
  --class-doc bpkg::pkg_bindist_common_options=exclude-base \
  --class-doc bpkg::pkg_bindist_debian_options=exclude-base \
  --class-doc bpkg::pkg_bindist_fedora_options=exclude-base \
  --class-doc bpkg::pkg_bindist_archive_options=exclude-base

# NOTE: remember to update a similar list in buildfile and bpkg.cli as well as
# the help topics sections in bpkg/buildfile and help.cxx.
#
pages="cfg-create cfg-info cfg-link cfg-unlink help pkg-clean pkg-configure \
pkg-disfigure pkg-drop pkg-fetch pkg-checkout pkg-install pkg-purge pkg-status \
pkg-test pkg-uninstall pkg-unpack pkg-update pkg-verify rep-add rep-remove \
rep-list rep-create rep-fetch rep-info repository-signing repository-types \
argument-grouping default-options-files"

for p in $pages; do
  compile $p $o
done

# Manual.
#

# @@ Note that we now have --ascii-tree CLI option.
#
function xhtml_to_ps () # <from> <to> [<html2ps-options>]
{
  local from="$1"
  shift
  local to="$1"
  shift

  sed -e 's/├/|/g' -e 's/│/|/g' -e 's/─/-/g' -e 's/└/\xb7/g' "$from" | \
  html2ps "${@}" -o "$to"
}

cli -I .. \
-v version="$(echo "$version" | sed -e 's/^\([^.]*\.[^.]*\).*/\1/')" \
-v date="$date" \
-v copyright="$copyright" \
--generate-html --html-suffix .xhtml \
--html-prologue-file doc-prologue.xhtml \
--html-epilogue-file doc-epilogue.xhtml \
--link-regex '%b([-.].+)%../../build2/doc/b$1%' \
--link-regex '%b(#.+)?%../../build2/doc/build2-build-system-manual.xhtml$1%' \
--link-regex '%bbot(#.+)?%../../bbot/doc/build2-build-bot-manual.xhtml$1%' \
--output-prefix build2-package-manager- \
manual.cli

xhtml_to_ps build2-package-manager-manual.xhtml build2-package-manager-manual-a4.ps -f doc.html2ps:a4.html2ps
ps2pdf14 -sPAPERSIZE=a4 -dOptimize=true -dEmbedAllFonts=true build2-package-manager-manual-a4.ps build2-package-manager-manual-a4.pdf

xhtml_to_ps build2-package-manager-manual.xhtml build2-package-manager-manual-letter.ps -f doc.html2ps:letter.html2ps
ps2pdf14 -sPAPERSIZE=letter -dOptimize=true -dEmbedAllFonts=true build2-package-manager-manual-letter.ps build2-package-manager-manual-letter.pdf
