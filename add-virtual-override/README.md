add-virtual-override
===============

About
-----
This tool adds `virtual` to all function declarations that are virtual, but
are not explicitly marked as such. It also adds the C++11 `override` keyword
to all functions that override virtual methods that are not marked `override`.

It is possible to have the tool use a different string instead of the
`override` keyword. For example, some code bases maintain compability with
compilers that don't support C++11 by using a macro like `OVERRIDE` which
can be compiled out when there's no support for the `override` keyword.
Use `add-virtual-override -override=OVERRIDE` for this behavior.

Usage
-----
    ./add-virtual-override <source0> [... <sourceN>] -- [additional clang args]

This will scan and fix each of the given source files, and overwrite the
changed files. This tool also supports using a compilation database to figure
out build options for each file, see
http://clang.llvm.org/docs/HowToSetupToolingForLLVM.html for more information.
