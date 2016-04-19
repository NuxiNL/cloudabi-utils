// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#ifndef EMULATE_H
#define EMULATE_H

#include <cloudabi_syscalls_struct.h>

void emulate(int, const void *, size_t, const cloudabi_syscalls_t *);

#endif
