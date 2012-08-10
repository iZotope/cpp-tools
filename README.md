cpp-tools
=========

Tools for analyzing and refactoring C++ source code.

Getting clang
-------------
These tools use clang. The easiest way to get it and build it is to
run the included shell scripts `get-clang.sh` and `build-clang.sh`. Note that
if you build it yourself, it should be compiled with C++11 support against
a standard library that also supports C++11. On Mac OS X, for example, this
means using libc++.

License
-------
These tools are all distributed under the BSD License. See the file LICENSE.md
for full license information. Copyright (c) 2012 iZotope Inc.
