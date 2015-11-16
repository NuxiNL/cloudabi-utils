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
  [`program_exec()` function](https://github.com/NuxiNL/cloudlibc/blob/master/src/include/program.h).
  This function can be used to start a CloudABI executable, providing it
  an argument data structure.

Below is a simple example (without any error handling or memory
management) that demonstrates how these APIs can be used to execute a
CloudABI process:

```c
#include <argdata.h>
#include <fcntl.h>
#include <program.h>

#ifndef O_EXEC
#define O_EXEC O_RDONLY
#endif

void start_my_executable(void) {
    int fd = open("/my/executable", O_EXEC);
    const argdata_t *keys[] = {
        argdata_create_str_c("first_name"),
        argdata_create_str_c("last_name"),
        argdata_create_str_c("age"),
    };
    const argdata_t *values[] = {
        argdata_create_str_c("Bobby"),
        argdata_create_str_c("Tables"),
        argdata_create_int(16),
    };
    program_exec(fd, argdata_create_map(keys, values, 3));
}
```

To ensure that this implementation conform to CloudABI's safety
requirements that it is safe to run untrusted programs, and that file
descriptors that are not referenced from the argument data structure are
never leaked into new processes, `program_exec()` depends on a small
proxy executable called `cloudabi-reexec`. This package ships with
prebuilt copies that were built by the
[CloudABI Ports Collection](https://github.com/NuxiNL/cloudabi-ports/tree/master/packages/cloudabi-reexec).

Finally, this package provides the `cloudabi-run` utility, which is
built on top of `libcloudabi`. This utility reads a YAML file from
`stdin`, converts it to an argument data structure, and uses that to
start a CloudABI executable. It provides some special tags to
automatically open files and network connections. Please refer to
`cloudabi-run`'s manual page for examples.

More details about CloudABI can be found on
[Nuxi's company webpage](https://nuxi.nl/).
