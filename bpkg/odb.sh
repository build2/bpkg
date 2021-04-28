#! /usr/bin/env bash

trap 'exit 1' ERR

odb=odb
inc=()

if test -d ../.bdep; then

  if [ -n "$1" ]; then
    cfg="$1"
  else
    # Use default configuration for headers.
    #
    cfg="$(bdep config list -d .. | \
sed -r -ne 's#^(@[^ ]+ )?([^ ]+)/ .*default.*$#\2#p')"
  fi

  inc+=("-I$(echo "$cfg"/libodb-[1-9]*/)")
  inc+=("-I$(echo "$cfg"/libodb-sqlite-[1-9]*/)")

  inc+=("-I$cfg/libbutl")
  inc+=("-I../../libbutl")

  inc+=("-I$cfg/libbpkg")
  inc+=("-I../../libbpkg")

  inc+=("-I$cfg/bpkg")
  inc+=("-I..")

else

  inc+=("-I$HOME/work/odb/builds/default/libodb-sqlite-default")
  inc+=("-I$HOME/work/odb/libodb-sqlite")

  inc+=("-I$HOME/work/odb/builds/default/libodb-default")
  inc+=("-I$HOME/work/odb/libodb")

  inc+=(-I.. -I../../libbpkg -I../../libbutl)

fi

$odb "${inc[@]}"                                                      \
    -DLIBODB_BUILD2 -DLIBODB_SQLITE_BUILD2 --generate-schema          \
    -d sqlite --std c++14 --generate-query                            \
    --odb-epilogue '#include <libbutl/small-vector-odb.hxx>' \
    --odb-epilogue '#include <bpkg/wrapper-traits.hxx>'               \
    --hxx-prologue '#include <libbutl/small-vector-odb.hxx>' \
    --hxx-prologue '#include <bpkg/wrapper-traits.hxx>'               \
    --hxx-prologue '#include <bpkg/value-traits.hxx>'                 \
    --include-with-brackets --include-prefix bpkg --guard-prefix BPKG \
    --schema main --schema-version-table main.schema_version          \
    --sqlite-override-null package.hxx
