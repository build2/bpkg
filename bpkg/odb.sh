#! /usr/bin/env bash

trap 'exit 1' ERR

odb=/home/boris/work/odb/odb/odb/odb
lib="\
-I/home/boris/work/odb/libodb-sqlite-default \
-I/home/boris/work/odb/libodb-sqlite \
-I/home/boris/work/odb/libodb-default \
-I/home/boris/work/odb/libodb"

$odb -d sqlite --std c++11 --hxx-suffix "" --generate-query --generate-schema \
  $lib -I.. -I../../libbpkg -I../../libbutl \
  --include-with-brackets --include-prefix bpkg --guard-prefix BPKG \
  --sqlite-override-null package
