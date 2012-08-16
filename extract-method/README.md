extract-method
===============

About
-----
This is a refactoring tool that can take some code, and automatically pull
it out into its own function, and replace the original code with a call to
that function. It is intended to be robust enough to handle more complex cases
where names are shadowed, or where member variables or member functions are
used.


Usage
-----
    ./extract-method <source> -first=[firstline] -last=[lastline] -name=[methodname]

This will take the code that starts on `firstline` and ends on `lastline` from
`source`, and refactor it into a new function that will be called `methodname`.
