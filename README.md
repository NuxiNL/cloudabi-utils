# Nuxi CloudABI utilities

This package contains a number of libraries and utilities that can be
used to easily start CloudABI programs. The `libcloudabi` library
provides native ports of some interfaces that are normally only
available as part of CloudABI's C library, namely:

* A portable implementation of CloudABI's
  [`<argdata.h>`](https://github.com/NuxiNL/cloudlibc/blob/master/src/include/argdata.h).
  This module provides functions that can be used to parse and construct
  argument data structures that are normally provided to CloudABI
  processes on startup, as a replacement for string command line
  arguments.

* A POSIX implementation of CloudABI's
  [`program_main()` function](https://github.com/NuxiNL/cloudlibc/blob/master/src/include/program.h).
  This function can be used to start a CloudABI executable, providing it
  an argument data structure.

To ensure that these implementations conform to CloudABI's safety
requirement that file descriptors that are not referenced from the
argument data structure are never leaked into new processes, this
implementation depends on a small proxy executable called
`cloudabi-reexec`. This package ships with prebuilt copies that were
built by the
[CloudABI Ports Collection](https://github.com/NuxiNL/cloudabi-ports/tree/master/packages/cloudabi-reexec).

Finally, this package provides the `cloudabi-run` utility, which is
built on top of `libcloudabi`. This utility reads a YAML file from
`stdin`, converts it to an argument data structure, and uses that to
start a CloudABI executable.

More details about CloudABI can be found on
[Nuxi's company webpage](https://nuxi.nl/).
