// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#ifndef TLS_H
#define TLS_H

#include <cloudabi_syscalls_struct.h>

// Bookkeeping for properly supporting TLS in guests.
struct tls {
  cloudabi_tcb_t tcb;  // Initial TLS area for new threads.
  void *tls_host;      // Backup of TLS area of the host while executing.
  const cloudabi_syscalls_t *forward;  // System calls to which to forward.
};

// System call table that properly switches TLS areas when entering and
// leaving system calls. Calls get forwarded to an alternative system
// call table.
extern cloudabi_syscalls_t tls_syscalls;

// Sets up TLS for a thread to point to an initial TLS area for a guest,
// while preserving the TLS area of the host.
void tls_init(struct tls *, const cloudabi_syscalls_t *);

#endif
