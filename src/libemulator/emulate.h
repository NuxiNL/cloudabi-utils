// Copyright (c) 2016-2018 Nuxi, https://nuxi.nl/
//
// SPDX-License-Identifier: BSD-2-Clause

#ifndef EMULATE_H
#define EMULATE_H

#include <cloudabi_syscalls_info.h>
#include <cloudabi_types.h>

struct syscalls {
#define wrapper(name)                                                         \
  CLOUDABI_SYSCALL_RETURNS_##name(                                            \
      cloudabi_errno_t, void) (*name)(CLOUDABI_SYSCALL_HAS_PARAMETERS_##name( \
      CLOUDABI_SYSCALL_PARAMETERS_##name, void));
  CLOUDABI_SYSCALL_NAMES(wrapper)
#undef wrapper
};

void emulate(int, const void *, size_t, const struct syscalls *);

#endif
