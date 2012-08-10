fix-unused-args
===============

About
-----
This is a tool for fixing warnings about unused arguments. It fixes these
warnings by commenting out the variable names. For example:

    int unused_arg(int x) {
      return 2;
    }

This will be transformed into:

    int unused_arg(int /*x*/) {
      return 2;
    }

Clearly this is not a useful tool to run every day on the same code; that
would defeat the purpose of the warning. This is useful for taking a large
existing codebase, and cleaning it up quickly so that it can compile with
warnings as errors. Then, future unused argument warnings will be dealt with.

Usage
-----
    ./fix-unused-args <source0> [... <sourceN>] -- [additional clang args]

This will scan and fix each of the given source files, and overwrite the
changed files. This tool also supports using a compilation database to figure
out build options for each file, see
http://clang.llvm.org/docs/HowToSetupToolingForLLVM.html for more information.
