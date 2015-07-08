// Copyright (c) 2015 Nuxi, https://nuxi.nl/
//
// This file is distrbuted under a 2-clause BSD license.
// See the LICENSE file for details.

// cloudabi-reexec - execute a program indirectly
//
// The cloudabi-reexec utility is used by cloudabi-run to execute the
// requested binary. It is invoked with a sequence containing the file
// descriptor of the program that needs to be executed and its argument
// data.
//
// The idea behind cloudabi-reexec is that by running a CloudABI
// executable already before executing the program, we already instruct
// the kernel to place the process in capabilities mode. Furthermore,
// the CloudABI exec() call prevents leaking file descriptors to the new
// process by accident.
//
// When the program is unable to start the target executable, it writes
// its error message to file descriptor 2, stderr. This is safe, as
// cloudabi-run also writes diagnostic messages to this file descriptor.

#include <argdata.h>
#include <program.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fd = -1;

static bool iterate(const argdata_t *ad, void *thunk) {
  if (fd < 0) {
    // First element in sequence: a file descriptor of the executable.
    if (argdata_get_fd(ad, &fd) != 0)
      _Exit(127);
    return true;
  } else {
    // Second element in sequence; arguments data for the executable.
    int error = exec(fd, ad);
    dprintf(2, "Failed to start executable: %s\n", strerror(error));
    _Exit(127);
  }
}

void program_main(const argdata_t *ad) {
  argdata_iterate_seq(ad, NULL, iterate);
  _Exit(127);
}
