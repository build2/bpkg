# file      : buildfile
# copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = bpkg/ doc/
./: $d doc{INSTALL LICENSE README version} file{manifest}
include $d

doc{INSTALL*}: install = false
