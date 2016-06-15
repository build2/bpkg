#! /usr/bin/env bash

trap 'exit 1' ERR

odb=odb
lib="\
-I$HOME/work/odb/libodb-sqlite-default \
-I$HOME/work/odb/libodb-sqlite \
-I$HOME/work/odb/libodb-default \
-I$HOME/work/odb/libodb"

$odb $lib -I.. -I../../libbpkg -I../../libbutl \
  -d sqlite --std c++11 --hxx-suffix "" --generate-query --generate-schema \
  --odb-epilogue '#include <bpkg/wrapper-traits>' \
  --hxx-prologue '#include <bpkg/wrapper-traits>' \
  --include-with-brackets --include-prefix bpkg --guard-prefix BPKG \
  --sqlite-override-null package
