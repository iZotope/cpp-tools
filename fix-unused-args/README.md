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

It's also possible to have this tool fix unused arguments in a way other
than commenting out names. For example, it might be helpful to distinguish
cases that were fixed by a tool from those that were fixed by humans, because
humans can judge whether there was a bug in the first place, while the tool
cannot. One way to do this is with a macro. As an example, this define can be
added to the code:

    #define TOOL_UNUSED_ARG(x)

Then, the tool can be run with the `-unused-prefix` and `-unused-suffix`
options:

    ./fix-unused-args <source0> [... <sourceN>] -unused-prefix="TOOL_UNUSED_ARG(" -unused-suffix=")" -- [additional clang args]

The tool just uses these arguments directly, putting them before and after
the names of unused arguments. Under a workflow like this, an engineer
who reviews the code later knows that the argument was made unnamed by a tool,
and there still might be a bug lurking.
