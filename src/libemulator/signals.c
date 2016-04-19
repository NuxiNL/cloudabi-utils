// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#include <signal.h>
#include <stddef.h>

#include "signals.h"

void signals_init(void) {
#ifdef SIG_DFL
  // Signals that need to be reset to the default state.
  const int defaultsigs[] = {
      SIGABRT, SIGALRM,   SIGBUS,  SIGCHLD, SIGCONT, SIGFPE,  SIGHUP,
      SIGILL,  SIGINT,    SIGKILL, SIGQUIT, SIGSEGV, SIGSTOP, SIGSYS,
      SIGTERM, SIGTRAP,   SIGTSTP, SIGTTIN, SIGTTOU, SIGURG,  SIGUSR1,
      SIGUSR2, SIGVTALRM, SIGXCPU, SIGXFSZ,
  };
  for (size_t i = 0; i < sizeof(defaultsigs) / sizeof(defaultsigs[0]); ++i) {
    struct sigaction sa = {
        .sa_handler = SIG_DFL,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(defaultsigs[i], &sa, NULL);
  }
#endif

#ifdef SIG_IGN
  // Signals that need to be ignored.
  const int ignoresigs[] = {SIGPIPE};
  for (size_t i = 0; i < sizeof(ignoresigs) / sizeof(ignoresigs[0]); ++i) {
    struct sigaction sa = {
        .sa_handler = SIG_IGN,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(ignoresigs[i], &sa, NULL);
  }
#endif
}
