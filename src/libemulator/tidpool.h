// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#ifndef TIDPOOL_H
#define TIDPOOL_H

#include <cloudabi_types.h>

// Allocates a new thread identifier.
cloudabi_tid_t tidpool_allocate(void);

// Should be invoked after forking, to reset the pool state.
void tidpool_postfork(void);

#endif
