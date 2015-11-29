# file      : buildfile
# copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = bpkg/
./: $d doc{LICENSE} file{version}
include $d
