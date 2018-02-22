#! /usr/bin/env bash

version=0.7.0-a.0.z
date="$(date +"%B %Y")"

trap 'exit 1' ERR
set -o errtrace # Trap in functions.

function info () { echo "$*" 1>&2; }
function error () { info "$*"; exit 1; }

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

  cli -I .. -v project="bpkg" -v version="$version" -v date="$date" \
--include-base-last "${o[@]}" --generate-html --html-prologue-file \
man-prologue.xhtml --html-epilogue-file man-epilogue.xhtml --html-suffix .xhtml \
--link-regex '%bpkg(#.+)?%build2-package-manager-manual.xhtml$1%' \
../bpkg/$n.cli

  cli -I .. -v project="bpkg" -v version="$version" -v date="$date" \
--include-base-last "${o[@]}" --generate-man --man-prologue-file \
man-prologue.1 --man-epilogue-file man-epilogue.1 --man-suffix .1 \
--link-regex '%bpkg(#.+)?%$1%' \
../bpkg/$n.cli
}

o="--output-prefix bpkg- --class-doc bpkg::common_options=short"

# A few special cases.
#
compile "common" $o --output-suffix "-options" --class-doc bpkg::common_options=long
compile "bpkg" $o --output-prefix "" --suppress-undocumented --class-doc bpkg::commands=short --class-doc bpkg::topics=short

pages="cfg-create help pkg-build pkg-clean pkg-configure pkg-disfigure \
pkg-drop pkg-fetch pkg-checkout pkg-install pkg-purge pkg-status pkg-test \
pkg-uninstall pkg-unpack pkg-update pkg-verify rep-add rep-remove rep-list \
rep-create rep-fetch rep-info repository-signing"

for p in $pages; do
  compile $p $o
done

# Manual.
#
cli -I .. \
-v version="$(echo "$version" | sed -e 's/^\([^.]*\.[^.]*\).*/\1/')" \
-v date="$date" \
--generate-html --html-suffix .xhtml \
--html-prologue-file doc-prologue.xhtml \
--html-epilogue-file doc-epilogue.xhtml \
--link-regex '%b([-.].+)%../../build2/doc/b$1%' \
--link-regex '%build2(#.+)?%../../build2/doc/build2-build-system-manual.xhtml$1%' \
--output-prefix build2-package-manager- manual.cli

html2ps -f doc.html2ps:a4.html2ps -o build2-package-manager-manual-a4.ps build2-package-manager-manual.xhtml
ps2pdf14 -sPAPERSIZE=a4 -dOptimize=true -dEmbedAllFonts=true build2-package-manager-manual-a4.ps build2-package-manager-manual-a4.pdf

html2ps -f doc.html2ps:letter.html2ps -o build2-package-manager-manual-letter.ps build2-package-manager-manual.xhtml
ps2pdf14 -sPAPERSIZE=letter -dOptimize=true -dEmbedAllFonts=true build2-package-manager-manual-letter.ps build2-package-manager-manual-letter.pdf
