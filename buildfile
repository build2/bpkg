# file      : buildfile
# copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = bpkg/
./: $d doc{INSTALL LICENSE version} file{manifest}
include $d

doc{INSTALL*}: install = false
