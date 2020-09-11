# NOTE: This project is unmaintained

CloudABI is no longer being maintained. It was an awesome experiment,
but it never got enough traction to be sustainable. If you like the idea
behind CloudABI, please consider looking into the
[WebAssembly System Interface (WASI)](https://wasi.dev). WASI's design
has been inspired by CloudABI.

The author of CloudABI (Ed Schouten) would like to thank all of the
people who contributed to this project. Let's hope CloudABI lives on in
spirit and influences the way software is designed in the future!

# Nuxi CloudABI utilities

This package contains a number of libraries and utilities that can be
used to easily start CloudABI programs.

The `libcloudabi` library provides a native port of the `program_exec()`
function. This is the function that can normally be used by CloudABI
programs to start new executables, similar to the POSIX `fexecve()`
function.

Whereas `fexecve()` uses simple string command line arguments,
`program_exec()` uses a YAML-like tree structure, called
[`argdata`](https://github.com/NuxiNL/argdata). What is special about
`argdata`, is that file descriptors are a native primitive data type;
they can be attached as leaves to the tree directly. This makes the way
of inheriting file descriptors during process execution a lot more
structured. It replaces concepts like close-on-exec flags entirely.

To ensure that this implementation of `program_exec()` conforms to
CloudABI's safety requirements (i.e., that it is safe to run untrusted
programs, and that file descriptors that are not referenced by the
`argdata` structure are not leaked into new processes),
`program_exec()` depends on a small proxy executable called
`cloudabi-reexec`. This package ships with prebuilt copies that were
built by the
[CloudABI Ports Collection](https://github.com/NuxiNL/cloudabi-ports/blob/master/packages/cloudabi-utils/BUILD).

Finally, this package provides the `cloudabi-run` utility, which is
built on top of `libcloudabi`. This utility reads a YAML file from
`stdin`, converts it to an `argdata`, and uses that to start a CloudABI
executable. It provides some special tags to automatically open files
and network connections. `cloudabi-run` also includes an emulator that
can be used to run CloudABI executables on operating systems for which
no native support is provided. Please refer to `cloudabi-run`'s manual
page for more details and examples.

More details about CloudABI can be found on
[Nuxi's company webpage](https://nuxi.nl/).
