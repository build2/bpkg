#!/bin/sh

# Some commonly useful addtional options that can be specified via the
# command line:
#
# --dry-run
# --progress
#
rsync -v -rlpt --exclude '.*' --copy-unsafe-links --prune-empty-dirs \
--delete-after $* 1/ pkg.cppget.org:/var/bpkg/tests/1/