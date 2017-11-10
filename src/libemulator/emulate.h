// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// SPDX-License-Identifier: BSD-2-Clause

#ifndef EMULATE_H
#define EMULATE_H

#include <cloudabi_syscalls_struct.h>

void emulate(int, const void *, size_t, const cloudabi_syscalls_t *);

#endif
