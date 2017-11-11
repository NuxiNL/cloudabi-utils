// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// SPDX-License-Identifier: BSD-2-Clause

#ifndef TIDPOOL_H
#define TIDPOOL_H

#include <cloudabi_types.h>

// Allocates a new thread identifier.
cloudabi_tid_t tidpool_allocate(void);

#endif
