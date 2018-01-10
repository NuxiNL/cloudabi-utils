// Copyright (c) 2016-2018 Nuxi, https://nuxi.nl/
//
// SPDX-License-Identifier: BSD-2-Clause

#ifndef TLS_H
#define TLS_H

#include <cloudabi_types.h>

struct syscalls;

// Bookkeeping for properly supporting TLS in guests.
struct tls {
  cloudabi_tcb_t tcb;  // Initial TLS area for new threads.
  void *tls_host;      // Backup of TLS area of the host while executing.
  const struct syscalls *forward;  // System calls to which to forward.
};

// System call table that properly switches TLS areas when entering and
// leaving system calls. Calls get forwarded to an alternative system
// call table.
extern struct syscalls tls_syscalls;

// Sets up TLS for a thread to point to an initial TLS area for a guest,
// while preserving the TLS area of the host.
void tls_init(struct tls *, const struct syscalls *);

#endif
