// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#include <stdatomic.h>

#include "tidpool.h"

// Start counting at two, as zero and one are reserved by the futex code
// (LOCK_MANAGED, LOCK_OWNER_UNKNOWN).
static _Atomic(cloudabi_tid_t) tidpool = 2;

cloudabi_tid_t tidpool_allocate(void) {
  // TODO(ed): Deal with overflows. But then again, thread identifiers
  // are 30 bits. Who ever creates more than one billion threads during
  // the lifetime of a single process? Some people do, I guess.
  return atomic_fetch_add_explicit(&tidpool, 1, memory_order_relaxed);
}

void tidpool_postfork(void) {
  atomic_init(&tidpool, 2);
}
