#! /usr/bin/env bash

trap 'exit 1' ERR

odb=/home/boris/work/odb/odb/odb/odb
lib="\
-I/home/boris/work/odb/libodb-sqlite-default \
-I/home/boris/work/odb/libodb-sqlite \
-I/home/boris/work/odb/libodb-default \
-I/home/boris/work/odb/libodb"

$odb $lib -I.. -I../../libbpkg -I../../libbutl \
  -d sqlite --std c++11 --hxx-suffix "" --generate-query --generate-schema \
  --odb-epilogue '#include <bpkg/wrapper-traits>' \
  --hxx-prologue '#include <bpkg/wrapper-traits>' \
  --include-with-brackets --include-prefix bpkg --guard-prefix BPKG \
  --sqlite-override-null package
