// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// SPDX-License-Identifier: BSD-2-Clause

#ifndef FUTEX_H
#define FUTEX_H

#include <stdbool.h>
#include <stddef.h>

#include <cloudabi_types.h>

cloudabi_errno_t futex_op_condvar_signal(_Atomic(cloudabi_condvar_t) *,
                                         cloudabi_scope_t, cloudabi_nthreads_t);
cloudabi_errno_t futex_op_lock_unlock(cloudabi_tid_t,
                                      _Atomic(cloudabi_lock_t) *,
                                      cloudabi_scope_t);
bool futex_op_poll(cloudabi_tid_t, const cloudabi_subscription_t *,
                   cloudabi_event_t *, size_t, size_t *);
void futex_postfork(void);

#endif
