// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#include "config.h"

#include <signal.h>
#include <stdbool.h>

#include <cloudabi_syscalls_info.h>
#include <cloudabi_syscalls_struct.h>

#include "tls.h"

#if defined(__aarch64__)

static void *tls_get(void) {
  void *tcb;
  asm volatile("mrs %0, tpidr_el0" : "=r"(tcb));
  return tcb;
}

static void tls_set(const void *tcb) {
  asm volatile("msr tpidr_el0, %0" : : "r"(tcb));
}

#elif defined(__arm__)

static void *tls_get(void) {
  void *tcb;
  asm volatile("mrc p15, 0, %0, cr13, cr0, 2" : "=r"(tcb));
  return tcb;
}

static void tls_set(const void *tcb) {
  asm volatile("mcr p15, 0, %0, cr13, cr0, 2" : : "r"(tcb));
}

#elif defined(__i386__)

static void *tls_get(void) {
  void *tcb;
  asm volatile("mov %%gs:0, %0" : "=r"(tcb));
  return tcb;
}

static void tls_set(const void *tcb) {
  asm volatile("mov %0, %%gs:0" : : "r"(tcb));
}

#elif defined(__x86_64__) && CONFIG_TLS_USE_GSBASE

// Signal handler for when code tries to use %fs.
static void handle_sigsegv(int sig, siginfo_t *si, void *ucp) {
  ucontext_t *uc = ucp;
  unsigned char *p = (unsigned char *)uc->uc_mcontext->__ss.__rip;
  if (*p == 0x64) {
    // Instruction starts with 0x64, meaning it tries to access %fs. By
    // changing the first byte to 0x65, it uses %gs instead.
    *p = 0x65;
  } else if (*p == 0x65) {
    // Instruction has already been patched up, but it may well be the
    // case that this was done by another CPU core. There is nothing
    // else we can do than return and try again. This may cause us to
    // get stuck indefinitely.
  } else {
    // Segmentation violation on an instruction that does not try to
    // access %fs. Reset the handler to its default action, so that the
    // segmentation violation is rethrown.
    struct sigaction sa = {
        .sa_handler = SIG_DFL,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
  }
}

static void *tls_get(void) {
  void *tcb;
  asm volatile("mov %%gs:0, %0" : "=r"(tcb));
  return tcb;
}

static void tls_set(const void *tcb) {
  asm volatile("mov %0, %%gs:0" : : "r"(tcb));
}

#elif defined(__x86_64__)

static void *tls_get(void) {
  void *tcb;
  asm volatile("mov %%fs:0, %0" : "=r"(tcb));
  return tcb;
}

static void tls_set(const void *tcb) {
  asm volatile("mov %0, %%fs:0" : : "r"(tcb));
}

#else
#error "Unsupported architecture"
#endif

void tls_init(struct tls *tls, const cloudabi_syscalls_t *forward) {
#if defined(__x86_64__) && CONFIG_TLS_USE_GSBASE
  // On OS X there doesn't seem to be any way to modify the %fs base.
  // Let's use %gs instead. Install a signal handler for SIGSEGV to
  // dynamically patch up instructions that access %fs.
  static bool handler_set_up = false;
  if (!handler_set_up) {
    struct sigaction sa = {
        .sa_sigaction = handle_sigsegv, .sa_flags = SA_SIGINFO,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    handler_set_up = true;
  }
#endif

  tls->tcb.parent = tls;
  tls->tls_host = tls_get();
  tls->forward = forward;
  tls_set(&tls->tcb);
}

// Generates wrappers for every system call in the system call table,
// preserving and restoring TLS accordingly.
#define wrapper(name)                                                  \
  static CLOUDABI_SYSCALL_RETURNS_##name(cloudabi_errno_t, void)       \
      name(CLOUDABI_SYSCALL_HAS_PARAMETERS_##name(                     \
          CLOUDABI_SYSCALL_PARAMETERS_##name, void)) {                 \
    /* Preserve TLS of the guest and switch to the TLS of the host. */ \
    const cloudabi_tcb_t *tls_guest = tls_get();                       \
    struct tls *tls = tls_guest->parent;                               \
    tls_set(tls->tls_host);                                            \
                                                                       \
    CLOUDABI_SYSCALL_RETURNS_##name(cloudabi_errno_t error =, )        \
        tls->forward->name(CLOUDABI_SYSCALL_PARAMETER_NAMES_##name);   \
                                                                       \
    /* Preserve TLS of the host and switch to the TLS of the guest. */ \
    tls->tls_host = tls_get();                                         \
    tls_set(tls_guest);                                                \
                                                                       \
    CLOUDABI_SYSCALL_RETURNS_##name(return error;, )                   \
  }
CLOUDABI_SYSCALL_NAMES(wrapper)
#undef wrapper

cloudabi_syscalls_t tls_syscalls = {
#define entry(name) .name = name,
    CLOUDABI_SYSCALL_NAMES(entry)
#undef entry
};
