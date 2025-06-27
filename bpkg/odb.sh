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

  # Note: there is nothing generated in libbutl-odb.
  #
  inc+=("-I../../libbutl/libbutl-odb")

  inc+=("-I$cfg/libbutl")
  inc+=("-I../../libbutl")

  inc+=("-I$cfg/libbpkg")
  inc+=("-I../../libbpkg")

  inc+=("-I$cfg/bpkg")
  inc+=("-I..")

else

  inc+=("-I../../libbutl/libbutl-odb")

  inc+=(-I.. -I../../libbpkg -I../../libbutl)

fi

$odb "${inc[@]}"                                                      \
    -DLIBODB_BUILD2 -DLIBODB_SQLITE_BUILD2                            \
    -d sqlite --std c++14 --generate-query --generate-prepared        \
    --generate-schema                                                 \
    --odb-epilogue '#include <libbutl/small-vector-odb.hxx>'          \
    --odb-epilogue '#include <bpkg/pointer-traits.hxx>'               \
    --odb-epilogue '#include <bpkg/wrapper-traits.hxx>'               \
    --hxx-prologue '#include <libbutl/small-vector-odb.hxx>'          \
    --hxx-prologue '#include <bpkg/pointer-traits.hxx>'               \
    --hxx-prologue '#include <bpkg/wrapper-traits.hxx>'               \
    --hxx-prologue '#include <bpkg/value-traits.hxx>'                 \
    --include-with-brackets --include-prefix bpkg --guard-prefix BPKG \
    --schema main --schema-version-table main.schema_version          \
    --sqlite-override-null package.hxx

$odb "${inc[@]}"                                                      \
    -DLIBODB_BUILD2 -DLIBODB_SQLITE_BUILD2                            \
    -d sqlite --std c++14 --generate-query                            \
    --generate-schema --schema-name 'fetch-cache'                     \
    --odb-epilogue '#include <bpkg/wrapper-traits.hxx>'               \
    --hxx-prologue '#include <bpkg/wrapper-traits.hxx>'               \
    --include-with-brackets --include-prefix bpkg --guard-prefix BPKG \
    --sqlite-override-null fetch-cache-data.hxx
