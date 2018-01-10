// Copyright (c) 2016-2018 Nuxi, https://nuxi.nl/
//
// SPDX-License-Identifier: BSD-2-Clause

#ifndef POSIX_H
#define POSIX_H

#include <stdbool.h>
#include <stddef.h>

#include "locking.h"

struct fd_entry;
struct syscalls;

struct fd_table {
  struct rwlock lock;
  struct fd_entry *entries;
  size_t size;
  size_t used;
};

extern _Thread_local cloudabi_tid_t curtid;

extern struct syscalls posix_syscalls;

void fd_table_init(struct fd_table *);
bool fd_table_insert_existing(struct fd_table *, cloudabi_fd_t, int);

#endif
