// Copyright (c) 2016-2017 Nuxi, https://nuxi.nl/
//
// SPDX-License-Identifier: BSD-2-Clause

// cloudabi-emulate - an emulator for CloudABI, running on CloudABI
//
// This small utility, having the same signature as cloudabi-reexec, is
// capable of emulating CloudABI executables. This tool has no practical
// use, apart from testing the portability of the emulator.

#include <argdata.h>
#include <program.h>
#include <stdlib.h>

#include "emulate.h"
#include "posix.h"

void program_main(const argdata_t *ad) {
  // Extract executable file descriptor and argument data from sequence.
  argdata_seq_iterator_t it;
  argdata_seq_iterate(ad, &it);
  const argdata_t *fdv, *argv;
  int fd;
  if (!argdata_seq_get(&it, &fdv) || argdata_get_fd(fdv, &fd) != 0)
    _Exit(127);
  argdata_seq_next(&it);
  if (!argdata_seq_get(&it, &argv))
    _Exit(127);

  // Serialize argument data that needs to be passed to the executable.
  size_t buflen, fdslen;
  argdata_serialized_length(argv, &buflen, &fdslen);
  int *fds = malloc(fdslen * sizeof(fds[0]) + buflen);
  if (fds == NULL)
    _Exit(127);
  void *buf = &fds[fdslen];
  fdslen = argdata_serialize(argv, buf, fds);

  // Register file descriptors.
  struct fd_table ft;
  fd_table_init(&ft);
  for (size_t i = 0; i < fdslen; ++i)
    if (!fd_table_insert_existing(&ft, i, fds[i]))
      _Exit(127);

  // Start emulation.
  emulate(fd, buf, buflen, &posix_syscalls);
  _Exit(127);
}
