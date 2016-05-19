// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#include "config.h"

#include <sys/types.h>
#if CONFIG_HAS_KQUEUE
#include <sys/event.h>
#endif
#include <sys/mman.h>
#if CONFIG_HAS_PDFORK
#include <sys/procdesc.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/un.h>
#if !CONFIG_HAS_PDFORK
#include <sys/wait.h>
#endif

#include <netinet/in.h>

#if CONFIG_HAS_MACH_ABSOLUTE_TIME
#include <mach/mach_time.h>
#endif

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cloudabi_syscalls_info.h>
#include <cloudabi_syscalls_struct.h>

#include "futex.h"
#include "locking.h"
#include "posix.h"
#include "random.h"
#include "refcount.h"
#include "rights.h"
#include "str.h"
#include "tidpool.h"
#include "tls.h"

// struct iovec must have the same layout as cloudabi_iovec_t.
static_assert(offsetof(struct iovec, iov_base) ==
                  offsetof(cloudabi_iovec_t, iov_base),
              "Offset mismatch");
static_assert(sizeof(((struct iovec *)0)->iov_base) ==
                  sizeof(((cloudabi_iovec_t *)0)->iov_base),
              "Size mismatch");
static_assert(offsetof(struct iovec, iov_len) ==
                  offsetof(cloudabi_iovec_t, iov_len),
              "Offset mismatch");
static_assert(sizeof(((struct iovec *)0)->iov_len) ==
                  sizeof(((cloudabi_iovec_t *)0)->iov_len),
              "Size mismatch");
static_assert(sizeof(struct iovec) == sizeof(cloudabi_iovec_t),
              "Size mismatch");

// struct iovec must have the same layout as cloudabi_ciovec_t.
static_assert(offsetof(struct iovec, iov_base) ==
                  offsetof(cloudabi_ciovec_t, iov_base),
              "Offset mismatch");
static_assert(sizeof(((struct iovec *)0)->iov_base) ==
                  sizeof(((cloudabi_ciovec_t *)0)->iov_base),
              "Size mismatch");
static_assert(offsetof(struct iovec, iov_len) ==
                  offsetof(cloudabi_ciovec_t, iov_len),
              "Offset mismatch");
static_assert(sizeof(((struct iovec *)0)->iov_len) ==
                  sizeof(((cloudabi_ciovec_t *)0)->iov_len),
              "Size mismatch");
static_assert(sizeof(struct iovec) == sizeof(cloudabi_ciovec_t),
              "Size mismatch");

// Current thread's file descriptor table.
static _Thread_local struct fd_table *curfds;
// Current thread's identifier.
_Thread_local cloudabi_tid_t curtid;

// Converts a POSIX error code to a CloudABI error code.
static cloudabi_errno_t convert_errno(int error) {
  static const cloudabi_errno_t errors[] = {
#define X(v) [v] = CLOUDABI_##v
    X(E2BIG),
    X(EACCES),
    X(EADDRINUSE),
    X(EADDRNOTAVAIL),
    X(EAFNOSUPPORT),
    X(EAGAIN),
    X(EALREADY),
    X(EBADF),
    X(EBADMSG),
    X(EBUSY),
    X(ECANCELED),
    X(ECHILD),
    X(ECONNABORTED),
    X(ECONNREFUSED),
    X(ECONNRESET),
    X(EDEADLK),
    X(EDESTADDRREQ),
    X(EDOM),
    X(EDQUOT),
    X(EEXIST),
    X(EFAULT),
    X(EFBIG),
    X(EHOSTUNREACH),
    X(EIDRM),
    X(EILSEQ),
    X(EINPROGRESS),
    X(EINTR),
    X(EINVAL),
    X(EIO),
    X(EISCONN),
    X(EISDIR),
    X(ELOOP),
    X(EMFILE),
    X(EMLINK),
    X(EMSGSIZE),
    X(EMULTIHOP),
    X(ENAMETOOLONG),
    X(ENETDOWN),
    X(ENETRESET),
    X(ENETUNREACH),
    X(ENFILE),
    X(ENOBUFS),
    X(ENODEV),
    X(ENOENT),
    X(ENOEXEC),
    X(ENOLCK),
    X(ENOLINK),
    X(ENOMEM),
    X(ENOMSG),
    X(ENOPROTOOPT),
    X(ENOSPC),
    X(ENOSYS),
#ifdef ENOTCAPABLE
    X(ENOTCAPABLE),
#endif
    X(ENOTCONN),
    X(ENOTDIR),
    X(ENOTEMPTY),
    X(ENOTRECOVERABLE),
    X(ENOTSOCK),
    X(ENOTSUP),
    X(ENOTTY),
    X(ENXIO),
    X(EOVERFLOW),
    X(EOWNERDEAD),
    X(EPERM),
    X(EPIPE),
    X(EPROTO),
    X(EPROTONOSUPPORT),
    X(EPROTOTYPE),
    X(ERANGE),
    X(EROFS),
    X(ESPIPE),
    X(ESRCH),
    X(ESTALE),
    X(ETIMEDOUT),
    X(ETXTBSY),
    X(EXDEV),
#undef X
#if EOPNOTSUPP != ENOTSUP
    [EOPNOTSUPP] = CLOUDABI_ENOTSUP,
#endif
#if EWOULDBLOCK != EAGAIN
    [EWOULDBLOCK] = CLOUDABI_EAGAIN,
#endif
  };
  if (error < 0 || error >= sizeof(errors) / sizeof(errors[0]) ||
      errors[error] == 0)
    return CLOUDABI_ENOSYS;
  return errors[error];
}

// Converts a POSIX timespec to a CloudABI timestamp.
static cloudabi_timestamp_t convert_timespec(const struct timespec *ts) {
  if (ts->tv_sec < 0)
    return 0;
  if (ts->tv_sec >= UINT64_MAX / 1000000000)
    return UINT64_MAX;
  return (cloudabi_timestamp_t)ts->tv_sec * 1000000000 + ts->tv_nsec;
}

#ifdef CLOCK_REALTIME

// Converts a CloudABI clock identifier to a POSIX clock identifier.
static bool convert_clockid(cloudabi_clockid_t in, clockid_t *out) {
  switch (in) {
    case CLOUDABI_CLOCK_MONOTONIC:
      *out = CLOCK_MONOTONIC;
      return true;
    case CLOUDABI_CLOCK_PROCESS_CPUTIME_ID:
      *out = CLOCK_PROCESS_CPUTIME_ID;
      return true;
    case CLOUDABI_CLOCK_REALTIME:
      *out = CLOCK_REALTIME;
      return true;
    case CLOUDABI_CLOCK_THREAD_CPUTIME_ID:
      *out = CLOCK_THREAD_CPUTIME_ID;
      return true;
    default:
      return false;
  }
}

static cloudabi_errno_t clock_res_get(cloudabi_clockid_t clock_id,
                                      cloudabi_timestamp_t *resolution) {
  clockid_t nclock_id;
  if (!convert_clockid(clock_id, &nclock_id))
    return CLOUDABI_EINVAL;
  struct timespec ts;
  if (clock_getres(nclock_id, &ts) < 0)
    return convert_errno(errno);
  *resolution = convert_timespec(&ts);
  return 0;
}

static cloudabi_errno_t clock_time_get(cloudabi_clockid_t clock_id,
                                       cloudabi_timestamp_t precision,
                                       cloudabi_timestamp_t *time) {
  clockid_t nclock_id;
  if (!convert_clockid(clock_id, &nclock_id))
    return CLOUDABI_EINVAL;
  struct timespec ts;
  if (clock_gettime(nclock_id, &ts) < 0)
    return convert_errno(errno);
  *time = convert_timespec(&ts);
  return 0;
}

#else

static cloudabi_errno_t clock_res_get(cloudabi_clockid_t clock_id,
                                      cloudabi_timestamp_t *resolution) {
  switch (clock_id) {
#if CONFIG_HAS_MACH_ABSOLUTE_TIME
    case CLOUDABI_CLOCK_MONOTONIC: {
      // Compute resolution based on mach_timebase_info()'s data once.
      static cloudabi_timestamp_t monotonic_resolution;
      if (monotonic_resolution == 0) {
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        cloudabi_timestamp_t res = timebase.numer / timebase.denom;
        monotonic_resolution = res > 0 ? res : 1;
      }
      *resolution = monotonic_resolution;
      return 0;
    }
#endif
    case CLOUDABI_CLOCK_PROCESS_CPUTIME_ID:
      *resolution = 1000000000 / CLOCKS_PER_SEC;
      return 0;
    case CLOUDABI_CLOCK_REALTIME:
      // The timeval structure provides time in microseconds.
      *resolution = 1000;
      return 0;
    default:
      return CLOUDABI_EINVAL;
  }
}

// Converts a POSIX timeval to a CloudABI timestamp.
static cloudabi_timestamp_t convert_timeval(const struct timeval *tv) {
  if (tv->tv_sec < 0)
    return 0;
  if (tv->tv_sec >= UINT64_MAX / 1000000000)
    return UINT64_MAX;
  return (cloudabi_timestamp_t)tv->tv_sec * 1000000000 + tv->tv_usec * 1000;
}

static cloudabi_errno_t clock_time_get(cloudabi_clockid_t clock_id,
                                       cloudabi_timestamp_t precision,
                                       cloudabi_timestamp_t *time) {
  switch (clock_id) {
#if CONFIG_HAS_MACH_ABSOLUTE_TIME
    case CLOUDABI_CLOCK_MONOTONIC: {
      uint64_t ticks = mach_absolute_time();
      static mach_timebase_info_data_t timebase;
      if (timebase.denom == 0)
        mach_timebase_info(&timebase);
      *time = ticks * timebase.numer / timebase.denom;
      return 0;
    }
#endif
    case CLOUDABI_CLOCK_PROCESS_CPUTIME_ID: {
      struct tms tms;
      if (times(&tms) == -1)
        return convert_errno(errno);
      static_assert(1000000000 / CLOCKS_PER_SEC * CLOCKS_PER_SEC == 1000000000,
                    "CLOCKS_PER_SEC needs to be a divisor of one billion");
      *time =
          (cloudabi_timestamp_t)tms.tms_utime * (1000000000 / CLOCKS_PER_SEC);
      return 0;
    }
    case CLOUDABI_CLOCK_REALTIME: {
      struct timeval tv;
      if (gettimeofday(&tv, NULL) < 0)
        return convert_errno(errno);
      *time = convert_timeval(&tv);
      return 0;
    }
    default:
      return CLOUDABI_EINVAL;
  }
}

#endif

static cloudabi_errno_t condvar_signal(_Atomic(cloudabi_condvar_t) * condvar,
                                       cloudabi_scope_t scope,
                                       cloudabi_nthreads_t nwaiters) {
  return futex_op_condvar_signal(condvar, scope, nwaiters);
}

struct fd_object {
  struct refcount refcount;
  cloudabi_filetype_t type;
  int number;

  union {
#if !CONFIG_HAS_PDFORK
    // Data associated with process descriptors.
    struct {
      pid_t pid;                     // Process ID of the child process.
      struct mutex lock;             // Lock to protect members below.
      bool terminated;               // Process has terminated.
      cloudabi_signal_t signal;      // If not zero, termination signal.
      cloudabi_exitcode_t exitcode;  // If signal is zero, exit code.
    } process;
#endif
    // Data associated with directory file descriptors.
    struct {
      struct mutex lock;            // Lock to protect members below.
      DIR *handle;                  // Directory handle.
      cloudabi_dircookie_t offset;  // Offset of the directory.
    } directory;
  };
};

struct fd_entry {
  struct fd_object *object;
  cloudabi_rights_t rights_base;
  cloudabi_rights_t rights_inheriting;
};

void fd_table_init(struct fd_table *ft) {
  rwlock_init(&ft->lock);
  ft->entries = NULL;
  ft->size = 0;
  ft->used = 0;
  curfds = ft;
}

// Looks up a file descriptor table entry by number and required rights.
static cloudabi_errno_t fd_table_get_entry(struct fd_table *ft,
                                           cloudabi_fd_t fd,
                                           cloudabi_rights_t rights_base,
                                           cloudabi_rights_t rights_inheriting,
                                           struct fd_entry **ret)
    REQUIRES_SHARED(ft->lock) {
  // Test for file descriptor existence.
  if (fd >= ft->size)
    return CLOUDABI_EBADF;
  struct fd_entry *fe = &ft->entries[fd];
  if (fe->object == NULL)
    return CLOUDABI_EBADF;

  // Validate rights.
  if ((~fe->rights_base & rights_base) != 0 ||
      (~fe->rights_inheriting & rights_inheriting) != 0)
    return CLOUDABI_ENOTCAPABLE;
  *ret = fe;
  return 0;
}

// Grows the file descriptor table to a required lower bound and a
// minimum number of free file descriptor table entries.
static bool fd_table_grow(struct fd_table *ft, size_t min, size_t incr)
    REQUIRES_EXCLUSIVE(ft->lock) {
  if (ft->size <= min || ft->size < (ft->used + incr) * 2) {
    // Keep on doubling the table size until we've met our constraints.
    size_t size = ft->size == 0 ? 1 : ft->size;
    while (size <= min || size < (ft->used + incr) * 2)
      size *= 2;

    // Grow the file descriptor table's allocation.
    struct fd_entry *entries = realloc(ft->entries, sizeof(*entries) * size);
    if (entries == NULL)
      return false;

    // Mark all new file descriptors as unused.
    for (size_t i = ft->size; i < size; ++i)
      entries[i].object = NULL;
    ft->entries = entries;
    ft->size = size;
  }
  return true;
}

// Allocates a new file descriptor object.
static cloudabi_errno_t fd_object_new(cloudabi_filetype_t type,
                                      struct fd_object **fo)
    TRYLOCKS_SHARED(0, (*fo)->refcount) {
  *fo = malloc(sizeof(**fo));
  if (*fo == NULL)
    return CLOUDABI_ENOMEM;
  refcount_init(&(*fo)->refcount, 1);
  (*fo)->type = type;
  (*fo)->number = -1;
  return 0;
}

// Attaches a file descriptor to the file descriptor table.
static void fd_table_attach(struct fd_table *ft, cloudabi_fd_t fd,
                            struct fd_object *fo, cloudabi_rights_t rights_base,
                            cloudabi_rights_t rights_inheriting)
    REQUIRES_EXCLUSIVE(ft->lock) CONSUMES(fo->refcount) {
  assert(ft->size > fd && "File descriptor table too small");
  struct fd_entry *fe = &ft->entries[fd];
  assert(fe->object == NULL && "Attempted to overwrite an existing descriptor");
  fe->object = fo;
  fe->rights_base = rights_base;
  fe->rights_inheriting = rights_inheriting;
  ++ft->used;
  assert(ft->size >= ft->used * 2 && "File descriptor too full");
}

// Detaches a file descriptor from the file descriptor table.
static void fd_table_detach(struct fd_table *ft, cloudabi_fd_t fd,
                            struct fd_object **fo) REQUIRES_EXCLUSIVE(ft->lock)
    PRODUCES((*fo)->refcount) {
  assert(ft->size > fd && "File descriptor table too small");
  struct fd_entry *fe = &ft->entries[fd];
  *fo = fe->object;
  assert(*fo != NULL && "Attempted to detach nonexistent descriptor");
  fe->object = NULL;
  assert(ft->used > 0 && "Reference count mismatch");
  --ft->used;
}

// Determines the type of a file descriptor and its maximum set of
// rights that should be attached to it.
static cloudabi_errno_t fd_determine_type_rights(
    int fd, cloudabi_filetype_t *type, cloudabi_rights_t *rights_base,
    cloudabi_rights_t *rights_inheriting) {
  struct stat sb;
  if (fstat(fd, &sb) < 0)
    return convert_errno(errno);
  if (S_ISBLK(sb.st_mode)) {
    *type = CLOUDABI_FILETYPE_BLOCK_DEVICE;
    *rights_base = RIGHTS_BLOCK_DEVICE_BASE;
    *rights_inheriting = RIGHTS_BLOCK_DEVICE_INHERITING;
  } else if (S_ISCHR(sb.st_mode)) {
    *type = CLOUDABI_FILETYPE_CHARACTER_DEVICE;
#if CONFIG_HAS_ISATTY
    if (isatty(fd)) {
      *rights_base = RIGHTS_TTY_BASE;
      *rights_inheriting = RIGHTS_TTY_INHERITING;
    } else
#endif
    {
      *rights_base = RIGHTS_CHARACTER_DEVICE_BASE;
      *rights_inheriting = RIGHTS_CHARACTER_DEVICE_INHERITING;
    }
  } else if (S_ISDIR(sb.st_mode)) {
    *type = CLOUDABI_FILETYPE_DIRECTORY;
    *rights_base = RIGHTS_DIRECTORY_BASE;
    *rights_inheriting = RIGHTS_DIRECTORY_INHERITING;
  } else if (S_ISFIFO(sb.st_mode)) {
    *type = CLOUDABI_FILETYPE_FIFO;
    *rights_base = RIGHTS_FIFO_BASE;
    *rights_inheriting = RIGHTS_FIFO_INHERITING;
  } else if (S_ISREG(sb.st_mode)) {
    *type = CLOUDABI_FILETYPE_REGULAR_FILE;
    *rights_base = RIGHTS_REGULAR_FILE_BASE;
    *rights_inheriting = RIGHTS_REGULAR_FILE_INHERITING;
  } else if (S_ISSOCK(sb.st_mode)) {
    int socktype;
    socklen_t socktypelen = sizeof(socktype);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &socktype, &socktypelen) < 0)
      return convert_errno(errno);
    switch (socktype) {
      case SOCK_DGRAM:
        *type = CLOUDABI_FILETYPE_SOCKET_DGRAM;
        break;
      case SOCK_SEQPACKET:
        *type = CLOUDABI_FILETYPE_SOCKET_SEQPACKET;
        break;
      case SOCK_STREAM:
        *type = CLOUDABI_FILETYPE_SOCKET_STREAM;
        break;
      default:
        return CLOUDABI_EINVAL;
    }
    *rights_base = RIGHTS_SOCKET_BASE;
    *rights_inheriting = RIGHTS_SOCKET_INHERITING;
#ifdef S_TYPEISPROC
  } else if (S_TYPEISPROC(&sb)) {
    *type = CLOUDABI_FILETYPE_PROCESS;
    *rights_base = RIGHTS_PROCESS_BASE;
    *rights_inheriting = RIGHTS_PROCESS_INHERITING;
#endif
#ifdef S_TYPEISSHM
  } else if (S_TYPEISSHM(&sb)) {
    *type = CLOUDABI_FILETYPE_SHARED_MEMORY;
    *rights_base = RIGHTS_SHARED_MEMORY_BASE;
    *rights_inheriting = RIGHTS_SHARED_MEMORY_INHERITING;
#endif
  } else {
    return CLOUDABI_EINVAL;
  }
  return 0;
}

// Returns the underlying file descriptor number of a file descriptor
// object. This function can only be applied to objects that have an
// underlying file descriptor number.
static int fd_number(const struct fd_object *fo) {
  int number = fo->number;
  assert(number >= 0 && "fd_number() called on virtual file descriptor");
  return number;
}

// Lowers the reference count on a file descriptor object. When the
// reference count reaches zero, its resources are cleaned up.
static void fd_object_release(struct fd_object *fo) UNLOCKS(fo->refcount) {
  if (refcount_release(&fo->refcount)) {
    switch (fo->type) {
      case CLOUDABI_FILETYPE_DIRECTORY:
        // For directories we may keep track of a DIR object. Calling
        // closedir() on it also closes the underlying file descriptor.
        mutex_destroy(&fo->directory.lock);
        if (fo->directory.handle == NULL) {
          close(fd_number(fo));
        } else {
          closedir(fo->directory.handle);
        }
        break;
#if !CONFIG_HAS_KQUEUE
      case CLOUDABI_FILETYPE_POLL:
        break;
#endif
#if !CONFIG_HAS_PDFORK
      case CLOUDABI_FILETYPE_PROCESS:
        // Closing a process descriptor should lead to termination of
        // the child process.
        mutex_destroy(&fo->process.lock);
        if (!fo->process.terminated) {
          kill(fo->process.pid, SIGSEGV);
          waitpid(fo->process.pid, NULL, 0);
        }
        break;
#endif
      default:
        close(fd_number(fo));
        break;
    }
    free(fo);
  }
}

// Inserts an already existing file descriptor into the file descriptor
// table.
bool fd_table_insert_existing(struct fd_table *ft, cloudabi_fd_t in, int out) {
  cloudabi_filetype_t type;
  cloudabi_rights_t rights_base, rights_inheriting;
  if (fd_determine_type_rights(out, &type, &rights_base, &rights_inheriting) !=
      0)
    return false;

  struct fd_object *fo;
  cloudabi_errno_t error = fd_object_new(type, &fo);
  if (error != 0)
    return false;
  fo->number = out;
  if (type == CLOUDABI_FILETYPE_DIRECTORY) {
    mutex_init(&fo->directory.lock);
    fo->directory.handle = NULL;
  }

  // Grow the file descriptor table if needed.
  rwlock_wrlock(&ft->lock);
  if (!fd_table_grow(ft, in, 1)) {
    rwlock_unlock(&ft->lock);
    fd_object_release(fo);
    return false;
  }

  fd_table_attach(ft, in, fo, rights_base, rights_inheriting);
  rwlock_unlock(&ft->lock);
  return true;
}

// Picks an unused slot from the file descriptor table.
static cloudabi_fd_t fd_table_unused(struct fd_table *ft)
    REQUIRES_SHARED(ft->lock) {
  assert(ft->size > ft->used && "File descriptor table has no free slots");
  for (;;) {
    cloudabi_fd_t fd = random_uniform(ft->size);
    if (ft->entries[fd].object == NULL)
      return fd;
  }
}

// Inserts a file descriptor object into an unused slot of the file
// descriptor table.
static cloudabi_errno_t fd_table_insert(struct fd_table *ft,
                                        struct fd_object *fo,
                                        cloudabi_rights_t rights_base,
                                        cloudabi_rights_t rights_inheriting,
                                        cloudabi_fd_t *out)
    REQUIRES_UNLOCKED(ft->lock) UNLOCKS(fo->refcount) {
  // Grow the file descriptor table if needed.
  rwlock_wrlock(&ft->lock);
  if (!fd_table_grow(ft, 0, 1)) {
    rwlock_unlock(&ft->lock);
    fd_object_release(fo);
    return convert_errno(errno);
  }

  *out = fd_table_unused(ft);
  fd_table_attach(ft, *out, fo, rights_base, rights_inheriting);
  rwlock_unlock(&ft->lock);
  return 0;
}

// Inserts a numerical file descriptor into the file descriptor table.
static cloudabi_errno_t fd_table_insert_fd(struct fd_table *ft, int in,
                                           cloudabi_filetype_t type,
                                           cloudabi_rights_t rights_base,
                                           cloudabi_rights_t rights_inheriting,
                                           cloudabi_fd_t *out)
    REQUIRES_UNLOCKED(ft->lock) {
  struct fd_object *fo;
  cloudabi_errno_t error = fd_object_new(type, &fo);
  if (error != 0) {
    close(in);
    return error;
  }
  fo->number = in;
  if (type == CLOUDABI_FILETYPE_DIRECTORY) {
    mutex_init(&fo->directory.lock);
    fo->directory.handle = NULL;
  }
  return fd_table_insert(ft, fo, rights_base, rights_inheriting, out);
}

// Inserts a pair of numerical file descriptors into the file descriptor
// table.
static cloudabi_errno_t fd_table_insert_fdpair(
    struct fd_table *ft, const int *in, cloudabi_filetype_t type,
    cloudabi_rights_t rights_base1, cloudabi_rights_t rights_base2,
    cloudabi_rights_t rights_inheriting, cloudabi_fd_t *out1,
    cloudabi_fd_t *out2) REQUIRES_UNLOCKED(ft->lock) {
  struct fd_object *fo1;
  cloudabi_errno_t error = fd_object_new(type, &fo1);
  if (error != 0) {
    close(in[0]);
    close(in[1]);
    return error;
  }
  fo1->number = in[0];
  struct fd_object *fo2;
  error = fd_object_new(type, &fo2);
  if (error != 0) {
    fd_object_release(fo1);
    close(in[1]);
    return error;
  }
  fo2->number = in[1];

  // Grow the file descriptor table if needed.
  rwlock_wrlock(&ft->lock);
  if (!fd_table_grow(ft, 0, 2)) {
    rwlock_unlock(&ft->lock);
    fd_object_release(fo1);
    fd_object_release(fo2);
    return convert_errno(errno);
  }

  *out1 = fd_table_unused(ft);
  fd_table_attach(ft, *out1, fo1, rights_base1, rights_inheriting);
  *out2 = fd_table_unused(ft);
  fd_table_attach(ft, *out2, fo2, rights_base2, rights_inheriting);
  rwlock_unlock(&ft->lock);
  return 0;
}

static cloudabi_errno_t fd_close(cloudabi_fd_t fd) {
  // Validate the file descriptor.
  struct fd_table *ft = curfds;
  rwlock_wrlock(&ft->lock);
  struct fd_entry *fe;
  cloudabi_errno_t error = fd_table_get_entry(ft, fd, 0, 0, &fe);
  if (error != 0) {
    rwlock_unlock(&ft->lock);
    return error;
  }

  // Remove it from the file descriptor table.
  struct fd_object *fo;
  fd_table_detach(ft, fd, &fo);
  rwlock_unlock(&ft->lock);
  fd_object_release(fo);
  return 0;
}

static cloudabi_errno_t fd_create_socket(cloudabi_filetype_t type, int socktype,
                                         cloudabi_fd_t *fd) {
  int nfd = socket(AF_UNIX, socktype, 0);
  if (nfd < 0)
    return convert_errno(errno);
  return fd_table_insert_fd(curfds, nfd, type, RIGHTS_SOCKET_BASE,
                            RIGHTS_SOCKET_INHERITING, fd);
}

static cloudabi_errno_t fd_create1(cloudabi_filetype_t type,
                                   cloudabi_fd_t *fd) {
  switch (type) {
    case CLOUDABI_FILETYPE_POLL: {
#if CONFIG_HAS_KQUEUE
      int nfd = kqueue();
      if (nfd < 0)
        return convert_errno(errno);
      return fd_table_insert_fd(curfds, nfd, CLOUDABI_FILETYPE_POLL,
                                RIGHTS_POLL_BASE, RIGHTS_POLL_INHERITING, fd);
#else
      struct fd_object *fo;
      cloudabi_errno_t error = fd_object_new(CLOUDABI_FILETYPE_POLL, &fo);
      if (error != 0)
        return error;
      return fd_table_insert(curfds, fo, RIGHTS_POLL_BASE,
                             RIGHTS_POLL_INHERITING, fd);
#endif
    }
    case CLOUDABI_FILETYPE_SHARED_MEMORY: {
#ifdef SHM_ANON
      int nfd = shm_open(SHM_ANON, O_RDWR, 0666);
      if (nfd < 0)
        return convert_errno(errno);
#else
      int nfd;
      for (;;) {
        unsigned int i;
        random_buf(&i, sizeof(i));
        char buf[64];
        snprintf(buf, sizeof(buf), "/anon%u", i);
        nfd = shm_open(buf, O_RDWR | O_EXCL | O_CREAT, 0700);
        if (nfd < 0) {
          if (errno == EEXIST)
            continue;
          return convert_errno(errno);
        }
        shm_unlink(buf);
        break;
      }
#endif
      return fd_table_insert_fd(curfds, nfd, type, RIGHTS_SHARED_MEMORY_BASE,
                                RIGHTS_SHARED_MEMORY_INHERITING, fd);
    }
    case CLOUDABI_FILETYPE_SOCKET_DGRAM:
      return fd_create_socket(type, SOCK_DGRAM, fd);
    case CLOUDABI_FILETYPE_SOCKET_SEQPACKET:
      return fd_create_socket(type, SOCK_SEQPACKET, fd);
    case CLOUDABI_FILETYPE_SOCKET_STREAM:
      return fd_create_socket(type, SOCK_STREAM, fd);
    default:
      return CLOUDABI_EINVAL;
  }
}

static cloudabi_errno_t fd_create_socketpair(cloudabi_filetype_t type,
                                             int socktype, cloudabi_fd_t *fd1,
                                             cloudabi_fd_t *fd2) {
  int fds[2];
  if (socketpair(AF_UNIX, socktype, 0, fds) < 0)
    return convert_errno(errno);
  return fd_table_insert_fdpair(curfds, fds, type, RIGHTS_SOCKET_BASE,
                                RIGHTS_SOCKET_BASE, RIGHTS_SOCKET_INHERITING,
                                fd1, fd2);
}

static cloudabi_errno_t fd_create2(cloudabi_filetype_t type, cloudabi_fd_t *fd1,
                                   cloudabi_fd_t *fd2) {
  switch (type) {
    case CLOUDABI_FILETYPE_FIFO: {
      int fds[2];
      if (pipe(fds) < 0)
        return convert_errno(errno);
      return fd_table_insert_fdpair(curfds, fds, type,
                                    RIGHTS_FIFO_BASE & ~CLOUDABI_RIGHT_FD_WRITE,
                                    RIGHTS_FIFO_BASE & ~CLOUDABI_RIGHT_FD_READ,
                                    RIGHTS_FIFO_INHERITING, fd1, fd2);
    }
    case CLOUDABI_FILETYPE_SOCKET_DGRAM:
      return fd_create_socketpair(type, SOCK_DGRAM, fd1, fd2);
    case CLOUDABI_FILETYPE_SOCKET_SEQPACKET:
      return fd_create_socketpair(type, SOCK_SEQPACKET, fd1, fd2);
    case CLOUDABI_FILETYPE_SOCKET_STREAM:
      return fd_create_socketpair(type, SOCK_STREAM, fd1, fd2);
    default:
      return CLOUDABI_EINVAL;
  }
}

// Temporarily locks the file descriptor table to look up a file
// descriptor object, increases its reference count and drops the lock.
static cloudabi_errno_t fd_object_get(struct fd_object **fo, cloudabi_fd_t fd,
                                      cloudabi_rights_t rights_base,
                                      cloudabi_rights_t rights_inheriting)
    TRYLOCKS_EXCLUSIVE(0, (*fo)->refcount) {
  // Test whether the file descriptor number is valid.
  struct fd_table *ft = curfds;
  rwlock_rdlock(&ft->lock);
  struct fd_entry *fe;
  cloudabi_errno_t error =
      fd_table_get_entry(ft, fd, rights_base, rights_inheriting, &fe);
  if (error != 0) {
    rwlock_unlock(&ft->lock);
    return error;
  }

  // Increase the reference count on the file descriptor object. A copy
  // of the rights are also stored, so callers can still access those if
  // needed.
  *fo = fe->object;
  refcount_acquire(&(*fo)->refcount);
  rwlock_unlock(&ft->lock);
  return 0;
}

static cloudabi_errno_t fd_datasync(cloudabi_fd_t fd) {
  struct fd_object *fo;
  cloudabi_errno_t error =
      fd_object_get(&fo, fd, CLOUDABI_RIGHT_FD_DATASYNC, 0);
  if (error != 0)
    return error;

#if CONFIG_HAS_FDATASYNC
  int ret = fdatasync(fd_number(fo));
#else
  int ret = fsync(fd_number(fo));
#endif
  fd_object_release(fo);
  if (ret < 0)
    return convert_errno(errno);
  return 0;
}

static cloudabi_errno_t fd_dup(cloudabi_fd_t from, cloudabi_fd_t *fd) {
  struct fd_table *ft = curfds;
  rwlock_wrlock(&ft->lock);
  struct fd_entry *fe;
  cloudabi_errno_t error = fd_table_get_entry(ft, from, 0, 0, &fe);
  if (error != 0) {
    rwlock_unlock(&ft->lock);
    return error;
  }

  // Grow the file descriptor table if needed.
  if (!fd_table_grow(ft, 0, 1)) {
    rwlock_unlock(&ft->lock);
    return convert_errno(errno);
  }

  // Attach it to a new place in the table.
  *fd = fd_table_unused(ft);
  refcount_acquire(&fe->object->refcount);
  fd_table_attach(ft, *fd, fe->object, fe->rights_base, fe->rights_inheriting);
  rwlock_unlock(&ft->lock);
  return 0;
}

static cloudabi_errno_t fd_pread(cloudabi_fd_t fd, const cloudabi_iovec_t *iov,
                                 size_t iovcnt, cloudabi_filesize_t offset,
                                 size_t *nread) {
  if (iovcnt == 0)
    return CLOUDABI_EINVAL;

  struct fd_object *fo;
  cloudabi_errno_t error = fd_object_get(
      &fo, fd, CLOUDABI_RIGHT_FD_READ | CLOUDABI_RIGHT_FD_SEEK, 0);
  if (error != 0)
    return error;

#if CONFIG_HAS_PREADV
  ssize_t len =
      preadv(fd_number(fo), (const struct iovec *)iov, iovcnt, offset);
  fd_object_release(fo);
  if (len < 0)
    return convert_errno(errno);
  *nread = len;
  return 0;
#else
  if (iovcnt == 1) {
    ssize_t len = pread(fd_number(fo), iov->iov_base, iov->iov_len, offset);
    fd_object_release(fo);
    if (len < 0)
      return convert_errno(errno);
    *nread = len;
    return 0;
  } else {
    // Allocate a single buffer to fit all data.
    size_t totalsize = 0;
    for (size_t i = 0; i < iovcnt; ++i)
      totalsize += iov[i].iov_len;
    char *buf = malloc(totalsize);
    if (buf == NULL) {
      fd_object_release(fo);
      return CLOUDABI_ENOMEM;
    }

    // Perform a single read operation.
    ssize_t len = pread(fd_number(fo), buf, totalsize, offset);
    fd_object_release(fo);
    if (len < 0) {
      free(buf);
      return convert_errno(errno);
    }

    // Copy data back to vectors.
    size_t bufoff = 0;
    for (size_t i = 0; i < iovcnt; ++i) {
      if (bufoff + iov[i].iov_len < len) {
        memcpy(iov[i].iov_base, buf + bufoff, iov[i].iov_len);
        bufoff += iov[i].iov_len;
      } else {
        memcpy(iov[i].iov_base, buf + bufoff, len - bufoff);
        break;
      }
    }
    free(buf);
    *nread = len;
    return 0;
  }
#endif
}

static cloudabi_errno_t fd_pwrite(cloudabi_fd_t fd,
                                  const cloudabi_ciovec_t *iov, size_t iovcnt,
                                  cloudabi_filesize_t offset,
                                  size_t *nwritten) {
  if (iovcnt == 0)
    return CLOUDABI_EINVAL;

  struct fd_object *fo;
  cloudabi_errno_t error = fd_object_get(
      &fo, fd, CLOUDABI_RIGHT_FD_WRITE | CLOUDABI_RIGHT_FD_SEEK, 0);
  if (error != 0)
    return error;

  ssize_t len;
#if CONFIG_HAS_PWRITEV
  len = pwritev(fd_number(fo), (const struct iovec *)iov, iovcnt, offset);
#else
  if (iovcnt == 1) {
    len = pwrite(fd_number(fo), iov->iov_base, iov->iov_len, offset);
  } else {
    // Allocate a single buffer to fit all data.
    size_t totalsize = 0;
    for (size_t i = 0; i < iovcnt; ++i)
      totalsize += iov[i].iov_len;
    char *buf = malloc(totalsize);
    if (buf == NULL) {
      fd_object_release(fo);
      return CLOUDABI_ENOMEM;
    }
    size_t bufoff = 0;
    for (size_t i = 0; i < iovcnt; ++i) {
      memcpy(buf + bufoff, iov[i].iov_base, iov[i].iov_len);
      bufoff += iov[i].iov_len;
    }

    // Perform a single write operation.
    len = pwrite(fd_number(fo), buf, totalsize, offset);
    free(buf);
  }
#endif
  fd_object_release(fo);
  if (len < 0)
    return convert_errno(errno);
  *nwritten = len;
  return 0;
}

static cloudabi_errno_t fd_read(cloudabi_fd_t fd, const cloudabi_iovec_t *iov,
                                size_t iovcnt, size_t *nread) {
  struct fd_object *fo;
  cloudabi_errno_t error = fd_object_get(&fo, fd, CLOUDABI_RIGHT_FD_READ, 0);
  if (error != 0)
    return error;

  ssize_t len = readv(fd_number(fo), (const struct iovec *)iov, iovcnt);
  fd_object_release(fo);
  if (len < 0)
    return convert_errno(errno);
  *nread = len;
  return 0;
}

static cloudabi_errno_t fd_replace(cloudabi_fd_t from, cloudabi_fd_t to) {
  struct fd_table *ft = curfds;
  rwlock_wrlock(&ft->lock);
  struct fd_entry *fe_from;
  cloudabi_errno_t error = fd_table_get_entry(ft, from, 0, 0, &fe_from);
  if (error != 0) {
    rwlock_unlock(&ft->lock);
    return error;
  }
  struct fd_entry *fe_to;
  error = fd_table_get_entry(ft, to, 0, 0, &fe_to);
  if (error != 0) {
    rwlock_unlock(&ft->lock);
    return error;
  }

  struct fd_object *fo;
  fd_table_detach(ft, to, &fo);
  refcount_acquire(&fe_from->object->refcount);
  fd_table_attach(ft, to, fe_from->object, fe_from->rights_base,
                  fe_from->rights_inheriting);
  rwlock_unlock(&ft->lock);
  fd_object_release(fo);
  return 0;
}

static cloudabi_errno_t fd_seek(cloudabi_fd_t fd, cloudabi_filedelta_t offset,
                                cloudabi_whence_t whence,
                                cloudabi_filesize_t *newoffset) {
  int nwhence;
  switch (whence) {
    case CLOUDABI_WHENCE_CUR:
      nwhence = SEEK_CUR;
      break;
    case CLOUDABI_WHENCE_END:
      nwhence = SEEK_END;
      break;
    case CLOUDABI_WHENCE_SET:
      nwhence = SEEK_SET;
      break;
    default:
      return CLOUDABI_EINVAL;
  }

  struct fd_object *fo;
  cloudabi_errno_t error = fd_object_get(
      &fo, fd, offset == 0 && whence == CLOUDABI_WHENCE_CUR
                   ? CLOUDABI_RIGHT_FD_TELL
                   : CLOUDABI_RIGHT_FD_SEEK | CLOUDABI_RIGHT_FD_TELL,
      0);
  if (error != 0)
    return error;

  off_t ret = lseek(fd_number(fo), offset, nwhence);
  fd_object_release(fo);
  if (ret < 0)
    return convert_errno(errno);
  *newoffset = ret;
  return 0;
}

static cloudabi_errno_t fd_stat_get(cloudabi_fd_t fd, cloudabi_fdstat_t *buf) {
  struct fd_table *ft = curfds;
  rwlock_rdlock(&ft->lock);
  struct fd_entry *fe;
  cloudabi_errno_t error = fd_table_get_entry(ft, fd, 0, 0, &fe);
  if (error != 0) {
    rwlock_unlock(&ft->lock);
    return error;
  }

  // Extract file descriptor type and rights.
  struct fd_object *fo = fe->object;
  *buf = (cloudabi_fdstat_t){
      .fs_filetype = fo->type,
      .fs_rights_base = fe->rights_base,
      .fs_rights_inheriting = fe->rights_inheriting,
  };

  // Fetch file descriptor flags.
  int ret;
  switch (fo->type) {
#if !CONFIG_HAS_KQUEUE
    case CLOUDABI_FILETYPE_POLL:
      ret = 0;
      break;
#endif
#if !CONFIG_HAS_PDFORK
    case CLOUDABI_FILETYPE_PROCESS:
      ret = 0;
      break;
#endif
    default:
      ret = fcntl(fd_number(fo), F_GETFL);
      break;
  }
  rwlock_unlock(&ft->lock);
  if (ret < 0)
    return convert_errno(errno);

  if ((ret & O_APPEND) != 0)
    buf->fs_flags |= CLOUDABI_FDFLAG_APPEND;
#ifdef O_DSYNC
  if ((ret & O_DSYNC) != 0)
    buf->fs_flags |= CLOUDABI_FDFLAG_DSYNC;
#endif
  if ((ret & O_NONBLOCK) != 0)
    buf->fs_flags |= CLOUDABI_FDFLAG_NONBLOCK;
#ifdef O_RSYNC
  if ((ret & O_RSYNC) != 0)
    buf->fs_flags |= CLOUDABI_FDFLAG_RSYNC;
#endif
  if ((ret & O_SYNC) != 0)
    buf->fs_flags |= CLOUDABI_FDFLAG_SYNC;
  return 0;
}

static cloudabi_errno_t fd_stat_put(cloudabi_fd_t fd,
                                    const cloudabi_fdstat_t *buf,
                                    cloudabi_fdsflags_t flags) {
  switch (flags) {
    case CLOUDABI_FDSTAT_FLAGS: {
      int noflags = 0;
      if ((buf->fs_flags & CLOUDABI_FDFLAG_APPEND) != 0)
        noflags |= O_APPEND;
      if ((buf->fs_flags & CLOUDABI_FDFLAG_DSYNC) != 0)
#ifdef O_DSYNC
        noflags |= O_DSYNC;
#else
        noflags |= O_SYNC;
#endif
      if ((buf->fs_flags & CLOUDABI_FDFLAG_NONBLOCK) != 0)
        noflags |= O_NONBLOCK;
      if ((buf->fs_flags & CLOUDABI_FDFLAG_RSYNC) != 0)
#ifdef O_RSYNC
        noflags |= O_RSYNC;
#else
        noflags |= O_SYNC;
#endif
      if ((buf->fs_flags & CLOUDABI_FDFLAG_SYNC) != 0)
        noflags |= O_SYNC;

      struct fd_object *fo;
      cloudabi_errno_t error =
          fd_object_get(&fo, fd, CLOUDABI_RIGHT_FD_STAT_PUT_FLAGS, 0);
      if (error != 0)
        return error;

      int ret = fcntl(fd_number(fo), F_SETFL, noflags);
      fd_object_release(fo);
      if (ret < 0)
        return convert_errno(errno);
      return 0;
    }
    case CLOUDABI_FDSTAT_RIGHTS: {
      struct fd_table *ft = curfds;
      rwlock_wrlock(&ft->lock);
      struct fd_entry *fe;
      cloudabi_rights_t base = buf->fs_rights_base;
      cloudabi_rights_t inheriting = buf->fs_rights_inheriting;
      cloudabi_errno_t error =
          fd_table_get_entry(ft, fd, base, inheriting, &fe);
      if (error != 0) {
        rwlock_unlock(&ft->lock);
        return error;
      }

      // Restrict the rights on the file descriptor.
      fe->rights_base = base;
      fe->rights_inheriting = inheriting;
      rwlock_unlock(&ft->lock);
      return 0;
    }
    default:
      return CLOUDABI_EINVAL;
  }
}

static cloudabi_errno_t fd_sync(cloudabi_fd_t fd) {
  struct fd_object *fo;
  cloudabi_errno_t error = fd_object_get(&fo, fd, CLOUDABI_RIGHT_FD_SYNC, 0);
  if (error != 0)
    return error;

  int ret = fsync(fd_number(fo));
  fd_object_release(fo);
  if (ret < 0)
    return convert_errno(errno);
  return 0;
}

static cloudabi_errno_t fd_write(cloudabi_fd_t fd, const cloudabi_ciovec_t *iov,
                                 size_t iovcnt, size_t *nwritten) {
  struct fd_object *fo;
  cloudabi_errno_t error = fd_object_get(&fo, fd, CLOUDABI_RIGHT_FD_WRITE, 0);
  if (error != 0)
    return error;

  ssize_t len = writev(fd_number(fo), (const struct iovec *)iov, iovcnt);
  fd_object_release(fo);
  if (len < 0)
    return convert_errno(errno);
  *nwritten = len;
  return 0;
}

static cloudabi_errno_t file_advise(cloudabi_fd_t fd,
                                    cloudabi_filesize_t offset,
                                    cloudabi_filesize_t len,
                                    cloudabi_advice_t advice) {
#ifdef POSIX_FADV_NORMAL
  int nadvice;
  switch (advice) {
    case CLOUDABI_ADVICE_DONTNEED:
      nadvice = POSIX_FADV_DONTNEED;
      break;
    case CLOUDABI_ADVICE_NOREUSE:
      nadvice = POSIX_FADV_NOREUSE;
      break;
    case CLOUDABI_ADVICE_NORMAL:
      nadvice = POSIX_FADV_NORMAL;
      break;
    case CLOUDABI_ADVICE_RANDOM:
      nadvice = POSIX_FADV_RANDOM;
      break;
    case CLOUDABI_ADVICE_SEQUENTIAL:
      nadvice = POSIX_FADV_SEQUENTIAL;
      break;
    case CLOUDABI_ADVICE_WILLNEED:
      nadvice = POSIX_FADV_WILLNEED;
      break;
    default:
      return CLOUDABI_EINVAL;
  }

  struct fd_object *fo;
  cloudabi_errno_t error =
      fd_object_get(&fo, fd, CLOUDABI_RIGHT_FILE_ADVISE, 0);
  if (error != 0)
    return error;

  int ret = posix_fadvise(fd_number(fo), offset, len, nadvice);
  fd_object_release(fo);
  if (ret != 0)
    return convert_errno(ret);
  return 0;
#else
  // Advisory information can safely be ignored if unsupported.
  switch (advice) {
    case CLOUDABI_ADVICE_DONTNEED:
    case CLOUDABI_ADVICE_NOREUSE:
    case CLOUDABI_ADVICE_NORMAL:
    case CLOUDABI_ADVICE_RANDOM:
    case CLOUDABI_ADVICE_SEQUENTIAL:
    case CLOUDABI_ADVICE_WILLNEED:
      break;
    default:
      return CLOUDABI_EINVAL;
  }

  // At least check for file descriptor existence.
  struct fd_table *ft = curfds;
  rwlock_rdlock(&ft->lock);
  struct fd_entry *fe;
  cloudabi_errno_t error =
      fd_table_get_entry(ft, fd, CLOUDABI_RIGHT_FILE_ADVISE, 0, &fe);
  rwlock_unlock(&ft->lock);
  return error;
#endif
}

static cloudabi_errno_t file_allocate(cloudabi_fd_t fd,
                                      cloudabi_filesize_t offset,
                                      cloudabi_filesize_t len) {
  struct fd_object *fo;
  cloudabi_errno_t error =
      fd_object_get(&fo, fd, CLOUDABI_RIGHT_FILE_ALLOCATE, 0);
  if (error != 0)
    return error;

#if CONFIG_HAS_POSIX_FALLOCATE
  int ret = posix_fallocate(fd_number(fo), offset, len);
#else
  // At least ensure that the file is grown to the right size.
  // TODO(ed): See if this can somehow be implemented without any race
  // conditions. We may end up shrinking the file right now.
  struct stat sb;
  int ret = fstat(fd_number(fo), &sb);
  if (ret == 0 && sb.st_size < offset + len)
    ret = ftruncate(fd_number(fo), offset + len);
#endif

  fd_object_release(fo);
  if (ret != 0)
    return convert_errno(ret);
  return 0;
}

// Reads the entire contents of a symbolic link, returning the contents
// in an allocated buffer. The allocated buffer is large enough to fit
// at least one extra byte, so the caller may append a trailing slash to
// it. This is needed by path_get().
static char *readlinkat_dup(int fd, const char *path) {
  char *buf = NULL;
  size_t len = 32;
  for (;;) {
    char *newbuf = realloc(buf, len);
    if (newbuf == NULL) {
      free(buf);
      return NULL;
    }
    buf = newbuf;
    ssize_t ret = readlinkat(fd, path, buf, len);
    if (ret < 0) {
      free(buf);
      return NULL;
    }
    if (ret + 1 < len) {
      buf[ret] = '\0';
      return buf;
    }
    len *= 2;
  }
}

// Lease to a directory, so a path underneath it can be accessed.
//
// This structure is used by system calls that operate on pathnames. In
// this environment, pathnames always consist of a pair of a file
// descriptor representing the directory where the lookup needs to start
// and the actual pathname string.
struct path_access {
  int fd;                       // Directory file descriptor.
  const char *path;             // Pathname.
  bool follow;                  // Whether symbolic links should be followed.
  char *path_start;             // Internal: pathname to free.
  struct fd_object *fd_object;  // Internal: directory file descriptor object.
};

// Creates a lease to a file descriptor and pathname pair. If the
// operating system does not implement Capsicum, it also normalizes the
// pathname to ensure the target path is placed underneath the
// directory.
static cloudabi_errno_t path_get(struct path_access *pa, cloudabi_lookup_t fd,
                                 const char *upath, size_t upathlen,
                                 cloudabi_rights_t rights_base,
                                 cloudabi_rights_t rights_inheriting,
                                 bool needs_final_component)
    TRYLOCKS_EXCLUSIVE(0, pa->fd_object->refcount) {
  char *path = str_nullterminate(upath, upathlen);
  if (path == NULL)
    return convert_errno(errno);

  // Fetch the directory file descriptor.
  struct fd_object *fo;
  cloudabi_errno_t error =
      fd_object_get(&fo, fd.fd, rights_base, rights_inheriting);
  if (error != 0) {
    free(path);
    return error;
  }

#if CONFIG_HAS_CAP_ENTER
  // Rely on the kernel to constrain access to automatically constrain
  // access to files stored underneath this directory.
  pa->fd = fd_number(fo);
  pa->path = pa->path_start = path;
  pa->follow = (fd.flags & CLOUDABI_LOOKUP_SYMLINK_FOLLOW) != 0;
  pa->fd_object = fo;
  return 0;
#else
  // The implementation provides no mechanism to constrain lookups to a
  // directory automatically. Emulate this logic by resolving the
  // pathname manually.

  // Stack of directory file descriptors. Index 0 always corresponds
  // with the directory provided to this function. Entering a directory
  // causes a file descriptor to be pushed, while handling ".." entries
  // causes an entry to be popped. Index 0 cannot be popped, as this
  // would imply escaping the base directory.
  int fds[128];
  fds[0] = fd_number(fo);
  size_t curfd = 0;

  // Stack of pathname strings used for symlink expansion. By using a
  // stack, there is no need to concatenate any pathname strings while
  // expanding symlinks.
  char *paths[32];
  char *paths_start[32];
  paths[0] = paths_start[0] = path;
  size_t curpath = 0;
  size_t expansions = 0;

  char *symlink;
  for (;;) {
    // Extract the next pathname component from 'paths[curpath]', null
    // terminate it and store it in 'file'. 'ends_with_slashes' stores
    // whether the pathname component is followed by one or more
    // trailing slashes, as this requires it to be a directory.
    char *file = paths[curpath];
    char *file_end = file + strcspn(file, "/");
    paths[curpath] = file_end + strspn(file_end, "/");
    bool ends_with_slashes = *file_end == '/';
    *file_end = '\0';

    // Test for empty pathname strings and absolute paths.
    if (file == file_end) {
      error = ends_with_slashes ? CLOUDABI_ENOTCAPABLE : CLOUDABI_ENOENT;
      goto fail;
    }

    if (strcmp(file, ".") == 0) {
      // Skip component.
    } else if (strcmp(file, "..") == 0) {
      // Pop a directory off the stack.
      if (curfd == 0) {
        // Attempted to go to parent directory of the directory file
        // descriptor.
        error = CLOUDABI_ENOTCAPABLE;
        goto fail;
      }
      close(fds[curfd--]);
    } else if (curpath > 0 || *paths[curpath] != '\0' ||
               (ends_with_slashes && !needs_final_component)) {
      // A pathname component whose name we're not interested in that is
      // followed by a slash or is followed by other pathname
      // components. In other words, a pathname component that must be a
      // directory. First attempt to obtain a directory file descriptor
      // for it.
      int newdir =
#ifdef O_SEARCH
          openat(fds[curfd], file, O_SEARCH | O_DIRECTORY | O_NOFOLLOW);
#else
          openat(fds[curfd], file, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
#endif
      if (newdir != -1) {
        // Success. Push it onto the directory stack.
        if (curfd + 1 == sizeof(fds) / sizeof(fds[0])) {
          close(newdir);
          error = CLOUDABI_ENAMETOOLONG;
          goto fail;
        }
        fds[++curfd] = newdir;
      } else {
        // Failed to open it. Attempt symlink expansion.
        if (errno != ELOOP && errno != EMLINK) {
          error = convert_errno(errno);
          goto fail;
        }
        symlink = readlinkat_dup(fds[curfd], file);
        if (symlink != NULL)
          goto push_symlink;
        error = convert_errno(errno);
        goto fail;
      }
    } else {
      // The final pathname component. Depending on whether it ends with
      // a slash or the symlink-follow flag is set, perform symlink
      // expansion.
      if (ends_with_slashes ||
          (fd.flags & CLOUDABI_LOOKUP_SYMLINK_FOLLOW) != 0) {
        symlink = readlinkat_dup(fds[curfd], file);
        if (symlink != NULL)
          goto push_symlink;
        if (errno != EINVAL && errno != ENOENT) {
          error = convert_errno(errno);
          goto fail;
        }
      }

      // Not a symlink, meaning we're done. Return the filename,
      // together with the directory containing this file.
      //
      // If the file was followed by a trailing slash, we must retain
      // it, to ensure system calls properly return ENOTDIR.
      // Unfortunately, this opens up a race condition, because this
      // means that users of path_get() will perform symlink expansion a
      // second time. There is nothing we can do to mitigate this, as
      // far as I know.
      if (ends_with_slashes)
        *file_end = '/';
      pa->path = file;
      pa->path_start = paths_start[0];
      goto success;
    }

    if (*paths[curpath] == '\0') {
      if (curpath == 0) {
        // No further pathname components to process. We may end up here
        // when called on paths like ".", "a/..", but also if the path
        // had trailing slashes and the caller is not interested in the
        // name of the pathname component.
        free(paths_start[0]);
        pa->path = ".";
        pa->path_start = NULL;
        goto success;
      }

      // Finished expanding symlink. Continue processing along the
      // original path.
      free(paths[curpath--]);
    }
    continue;

  push_symlink:
    // Prevent infinite loops by placing an upper limit on the number of
    // symlink expansions.
    if (++expansions == 128) {
      free(symlink);
      error = CLOUDABI_ELOOP;
      goto fail;
    }

    if (*paths[curpath] == '\0') {
      // The original path already finished processing. Replace it by
      // this symlink entirely.
      free(paths_start[curpath]);
    } else if (curpath + 1 == sizeof(paths) / sizeof(paths[0])) {
      // Too many nested symlinks. Stop processing.
      free(symlink);
      error = CLOUDABI_ELOOP;
      goto fail;
    } else {
      // The original path still has components left. Retain the
      // components that remain, so we can process them afterwards.
      ++curpath;
    }

    // Append a trailing slash to the symlink if the path leading up to
    // it also contained one. Otherwise we would not throw ENOTDIR if
    // the target is not a directory.
    if (ends_with_slashes)
      strcat(symlink, "/");
    paths[curpath] = paths_start[curpath] = symlink;
  }

success:
  // Return the lease. Close all directories, except the one the caller
  // needs to use.
  for (size_t i = 1; i < curfd; ++i)
    close(fds[i]);
  pa->fd = fds[curfd];
  pa->follow = false;
  pa->fd_object = fo;
  return 0;

fail:
  // Failure. Free all resources.
  for (size_t i = 1; i <= curfd; ++i)
    close(fds[i]);
  for (size_t i = 0; i <= curpath; ++i)
    free(paths_start[i]);
  fd_object_release(fo);
  return error;
#endif
}

static cloudabi_errno_t path_get_nofollow(
    struct path_access *pa, cloudabi_fd_t fd, const char *path, size_t pathlen,
    cloudabi_rights_t rights_base, cloudabi_rights_t rights_inheriting,
    bool needs_final_component) TRYLOCKS_EXCLUSIVE(0, pa->fd_object->refcount) {
  cloudabi_lookup_t lookup = {.fd = fd};
  return path_get(pa, lookup, path, pathlen, rights_base, rights_inheriting,
                  needs_final_component);
}

static void path_put(struct path_access *pa) UNLOCKS(pa->fd_object->refcount) {
  free(pa->path_start);
  if (fd_number(pa->fd_object) != pa->fd)
    close(pa->fd);
  fd_object_release(pa->fd_object);
}

#ifdef UTIME_NOW
#define HAS_UTIMENS 1
#else
#define HAS_UTIMENS 0
#endif

#define HAS_CWD_LOCK                                            \
  (!HAS_UTIMENS || !CONFIG_HAS_MKFIFOAT ||                      \
   !(CONFIG_HAS_BINDAT_SOCKADDR || CONFIG_HAS_BINDAT_STRING) || \
   !(CONFIG_HAS_CONNECTAT_SOCKADDR || CONFIG_HAS_CONNECTAT_STRING))

#if HAS_CWD_LOCK

static struct mutex cwd_lock = MUTEX_INITIALIZER;

// Switches to a new working directory while holding a global lock,
// ensuring that no other system calls modify the working directory in
// the meantime.
static cloudabi_errno_t cwd_get(const struct path_access *pa)
    TRYLOCKS_EXCLUSIVE(0, cwd_lock)
        REQUIRES_EXCLUSIVE(pa->fd_object->refcount) {
  mutex_lock(&cwd_lock);
  if (fchdir(pa->fd) < 0) {
    mutex_unlock(&cwd_lock);
    return convert_errno(errno);
  }
  return 0;
}

static void cwd_put(void) UNLOCKS(cwd_lock) {
  mutex_unlock(&cwd_lock);
}

#endif

static cloudabi_errno_t file_create(cloudabi_fd_t fd, const char *path,
                                    size_t pathlen, cloudabi_filetype_t type) {
  switch (type) {
    case CLOUDABI_FILETYPE_DIRECTORY: {
      struct path_access pa;
      cloudabi_errno_t error =
          path_get_nofollow(&pa, fd, path, pathlen,
                            CLOUDABI_RIGHT_FILE_CREATE_DIRECTORY, 0, true);
      if (error != 0)
        return error;

      int ret = mkdirat(pa.fd, pa.path, 0777);
      path_put(&pa);
      if (ret < 0)
        return convert_errno(errno);
      return 0;
    }
    case CLOUDABI_FILETYPE_FIFO: {
      struct path_access pa;
      cloudabi_errno_t error = path_get_nofollow(
          &pa, fd, path, pathlen, CLOUDABI_RIGHT_FILE_CREATE_FIFO, 0, true);
      if (error != 0)
        return error;

#if CONFIG_HAS_MKFIFOAT
      int ret = mkfifoat(pa.fd, pa.path, 0777);
#else
      error = cwd_get(&pa);
      if (error != 0) {
        path_put(&pa);
        return error;
      }
      int ret = mkfifo(pa.path, 0777);
      cwd_put();
#endif
      path_put(&pa);
      if (ret < 0)
        return convert_errno(errno);
      return 0;
    }
    default:
      return CLOUDABI_EINVAL;
  }
}

static cloudabi_errno_t file_link(cloudabi_lookup_t fd1, const char *path1,
                                  size_t path1len, cloudabi_fd_t fd2,
                                  const char *path2, size_t path2len) {
  struct path_access pa1;
  cloudabi_errno_t error = path_get(&pa1, fd1, path1, path1len,
                                    CLOUDABI_RIGHT_FILE_LINK_SOURCE, 0, false);
  if (error != 0)
    return error;

  struct path_access pa2;
  error = path_get_nofollow(&pa2, fd2, path2, path2len,
                            CLOUDABI_RIGHT_FILE_LINK_TARGET, 0, true);
  if (error != 0) {
    path_put(&pa1);
    return error;
  }

  int ret = linkat(pa1.fd, pa1.path, pa2.fd, pa2.path,
                   pa1.follow ? AT_SYMLINK_FOLLOW : 0);
  if (ret < 0 && errno == ENOTSUP && !pa1.follow) {
    // OS X doesn't allow creating hardlinks to symbolic links.
    // Duplicate the symbolic link instead.
    char *target = readlinkat_dup(pa1.fd, pa1.path);
    if (target != NULL) {
      ret = symlinkat(target, pa2.fd, pa2.path);
      free(target);
    }
  }
  path_put(&pa1);
  path_put(&pa2);
  if (ret < 0)
    return convert_errno(errno);
  return 0;
}

static cloudabi_errno_t file_open(cloudabi_lookup_t dirfd, const char *path,
                                  size_t pathlen, cloudabi_oflags_t oflags,
                                  const cloudabi_fdstat_t *fds,
                                  cloudabi_fd_t *fd) {
  // Rights that should be installed on the new file descriptor.
  cloudabi_rights_t rights_base = fds->fs_rights_base;
  cloudabi_rights_t rights_inheriting = fds->fs_rights_inheriting;

  // Which open() mode should be used to satisfy the needed rights.
  bool read =
      (rights_base & (CLOUDABI_RIGHT_FD_READ | CLOUDABI_RIGHT_FILE_READDIR |
                      CLOUDABI_RIGHT_MEM_MAP_EXEC)) != 0;
  bool write =
      (rights_base & (CLOUDABI_RIGHT_FD_DATASYNC | CLOUDABI_RIGHT_FD_WRITE |
                      CLOUDABI_RIGHT_FILE_ALLOCATE |
                      CLOUDABI_RIGHT_FILE_STAT_FPUT_SIZE)) != 0;
  int noflags = write ? read ? O_RDWR : O_WRONLY : O_RDONLY;

  // Which rights are needed on the directory file descriptor.
  cloudabi_rights_t needed_base = CLOUDABI_RIGHT_FILE_OPEN;
  cloudabi_rights_t needed_inheriting = rights_base | rights_inheriting;

  // Convert open flags.
  if ((oflags & CLOUDABI_O_CREAT) != 0) {
    noflags |= O_CREAT;
    needed_base |= CLOUDABI_RIGHT_FILE_CREATE_FILE;
  }
  if ((oflags & CLOUDABI_O_DIRECTORY) != 0)
    noflags |= O_DIRECTORY;
  if ((oflags & CLOUDABI_O_EXCL) != 0)
    noflags |= O_EXCL;
  if ((oflags & CLOUDABI_O_TRUNC) != 0) {
    noflags |= O_TRUNC;
    needed_inheriting |= CLOUDABI_RIGHT_FILE_STAT_FPUT_SIZE;
  }

  // Convert file descriptor flags.
  if ((fds->fs_flags & CLOUDABI_FDFLAG_APPEND) != 0)
    noflags |= O_APPEND;
  if ((fds->fs_flags & CLOUDABI_FDFLAG_DSYNC) != 0) {
#ifdef O_DSYNC
    noflags |= O_DSYNC;
#else
    noflags |= O_SYNC;
#endif
    needed_inheriting |= CLOUDABI_RIGHT_FD_DATASYNC;
  }
  if ((fds->fs_flags & CLOUDABI_FDFLAG_NONBLOCK) != 0)
    noflags |= O_NONBLOCK;
  if ((fds->fs_flags & CLOUDABI_FDFLAG_RSYNC) != 0) {
#ifdef O_RSYNC
    noflags |= O_RSYNC;
#else
    noflags |= O_SYNC;
#endif
    needed_inheriting |= CLOUDABI_RIGHT_FD_SYNC;
  }
  if ((fds->fs_flags & CLOUDABI_FDFLAG_SYNC) != 0) {
    noflags |= O_SYNC;
    needed_inheriting |= CLOUDABI_RIGHT_FD_SYNC;
  }
  if (write && (noflags & (O_APPEND | O_TRUNC)) == 0)
    needed_inheriting |= CLOUDABI_RIGHT_FD_SEEK;

  struct path_access pa;
  cloudabi_errno_t error =
      path_get(&pa, dirfd, path, pathlen, needed_base, needed_inheriting,
               (oflags & CLOUDABI_O_CREAT) != 0);
  if (error != 0)
    return error;
  if (!pa.follow)
    noflags |= O_NOFOLLOW;

  int nfd = openat(pa.fd, pa.path, noflags, 0777);
  if (nfd < 0) {
    // Linux returns ENXIO instead of EOPNOTSUPP when opening a socket.
    if (errno == ENXIO) {
      struct stat sb;
      int ret =
          fstatat(pa.fd, pa.path, &sb, pa.follow ? 0 : AT_SYMLINK_NOFOLLOW);
      path_put(&pa);
      return ret == 0 && S_ISSOCK(sb.st_mode) ? CLOUDABI_ENOTSUP
                                              : CLOUDABI_ENXIO;
    }
    path_put(&pa);
    // FreeBSD returns EMLINK instead of ELOOP when using O_NOFOLLOW on
    // a symlink.
    if (!pa.follow && errno == EMLINK)
      return CLOUDABI_ELOOP;
    return convert_errno(errno);
  }
  path_put(&pa);

  // Determine the type of the new file descriptor and which rights
  // contradict with this type.
  cloudabi_filetype_t type;
  cloudabi_rights_t max_base, max_inheriting;
  error = fd_determine_type_rights(nfd, &type, &max_base, &max_inheriting);
  if (error != 0) {
    close(nfd);
    return error;
  }
  return fd_table_insert_fd(curfds, nfd, type, rights_base & max_base,
                            rights_inheriting & max_inheriting, fd);
}

// Copies out directory entry metadata or filename, potentially
// truncating it in the process.
static void file_readdir_put(void *buf, size_t bufsize, size_t *bufused,
                             const void *elem, size_t elemsize) {
  size_t bufavail = bufsize - *bufused;
  if (elemsize > bufavail)
    elemsize = bufavail;
  memcpy((char *)buf + *bufused, elem, elemsize);
  *bufused += elemsize;
}

static cloudabi_errno_t file_readdir(cloudabi_fd_t fd, void *buf, size_t nbyte,
                                     cloudabi_dircookie_t cookie,
                                     size_t *bufused) {
  struct fd_object *fo;
  cloudabi_errno_t error =
      fd_object_get(&fo, fd, CLOUDABI_RIGHT_FILE_READDIR, 0);
  if (error != 0) {
    return error;
  }

  // Create a directory handle if none has been opened yet.
  mutex_lock(&fo->directory.lock);
  DIR *dp = fo->directory.handle;
  if (dp == NULL) {
    dp = fdopendir(fd_number(fo));
    if (dp == NULL) {
      mutex_unlock(&fo->directory.lock);
      fd_object_release(fo);
      return convert_errno(errno);
    }
    fo->directory.handle = dp;
    fo->directory.offset = CLOUDABI_DIRCOOKIE_START;
  }

  // Seek to the right position if the requested offset does not match
  // the current offset.
  if (fo->directory.offset != cookie) {
    if (cookie == CLOUDABI_DIRCOOKIE_START)
      rewinddir(dp);
    else
      seekdir(dp, cookie);
    fo->directory.offset = cookie;
  }

  *bufused = 0;
  while (nbyte > 0) {
    // Read the next directory entry.
    errno = 0;
    struct dirent *de = readdir(dp);
    if (de == NULL) {
      mutex_unlock(&fo->directory.lock);
      fd_object_release(fo);
      return errno == 0 || *bufused > 0 ? 0 : convert_errno(errno);
    }
    fo->directory.offset = telldir(dp);

    // Craft a directory entry and copy that back.
    size_t namlen = strlen(de->d_name);
    cloudabi_dirent_t cde = {
        .d_next = fo->directory.offset, .d_ino = de->d_ino, .d_namlen = namlen,
    };
    switch (de->d_type) {
      case DT_BLK:
        cde.d_type = CLOUDABI_FILETYPE_BLOCK_DEVICE;
        break;
      case DT_CHR:
        cde.d_type = CLOUDABI_FILETYPE_CHARACTER_DEVICE;
        break;
      case DT_DIR:
        cde.d_type = CLOUDABI_FILETYPE_DIRECTORY;
        break;
      case DT_FIFO:
        cde.d_type = CLOUDABI_FILETYPE_FIFO;
        break;
      case DT_LNK:
        cde.d_type = CLOUDABI_FILETYPE_SYMBOLIC_LINK;
        break;
      case DT_REG:
        cde.d_type = CLOUDABI_FILETYPE_REGULAR_FILE;
        break;
#ifdef DT_SOCK
      case DT_SOCK:
        // Technically not correct, but good enough.
        cde.d_type = CLOUDABI_FILETYPE_SOCKET_STREAM;
        break;
#endif
      default:
        cde.d_type = CLOUDABI_FILETYPE_UNKNOWN;
        break;
    }
    file_readdir_put(buf, nbyte, bufused, &cde, sizeof(cde));
    file_readdir_put(buf, nbyte, bufused, de->d_name, namlen);
  }
  mutex_unlock(&fo->directory.lock);
  fd_object_release(fo);
  return 0;
}

static cloudabi_errno_t file_readlink(cloudabi_fd_t fd, const char *path,
                                      size_t pathlen, char *buf, size_t bufsize,
                                      size_t *bufused) {
  struct path_access pa;
  cloudabi_errno_t error = path_get_nofollow(
      &pa, fd, path, pathlen, CLOUDABI_RIGHT_FILE_READLINK, 0, false);
  if (error != 0)
    return error;

  // Linux requires that the buffer size is positive. whereas POSIX does
  // not. Use a fake buffer to store the results if the size is zero.
  char fakebuf[1];
  ssize_t len = readlinkat(pa.fd, pa.path, bufsize == 0 ? fakebuf : buf,
                           bufsize == 0 ? sizeof(fakebuf) : bufsize);
  path_put(&pa);
  if (len < 0)
    return convert_errno(errno);
  *bufused = len < bufsize ? len : bufsize;
  return 0;
}

static cloudabi_errno_t file_rename(cloudabi_fd_t oldfd, const char *old,
                                    size_t oldlen, cloudabi_fd_t newfd,
                                    const char *new, size_t newlen) {
  struct path_access pa1;
  cloudabi_errno_t error = path_get_nofollow(
      &pa1, oldfd, old, oldlen, CLOUDABI_RIGHT_FILE_RENAME_SOURCE, 0, true);
  if (error != 0)
    return error;

  struct path_access pa2;
  error = path_get_nofollow(&pa2, newfd, new, newlen,
                            CLOUDABI_RIGHT_FILE_RENAME_TARGET, 0, true);
  if (error != 0) {
    path_put(&pa1);
    return error;
  }

  int ret = renameat(pa1.fd, pa1.path, pa2.fd, pa2.path);
  path_put(&pa1);
  path_put(&pa2);
  if (ret < 0) {
    // Linux returns EBUSY in cases where EINVAL would be more suited.
    return errno == EBUSY ? CLOUDABI_EINVAL : convert_errno(errno);
  }
  return 0;
}

// Converts a POSIX stat structure to a CloudABI filestat structure.
static void convert_stat(const struct stat *in, cloudabi_filestat_t *out) {
  *out = (cloudabi_filestat_t){
      .st_dev = in->st_dev,
      .st_ino = in->st_ino,
      .st_nlink = in->st_nlink,
      .st_size = in->st_size,
      .st_atim = convert_timespec(&in->st_atim),
      .st_mtim = convert_timespec(&in->st_mtim),
      .st_ctim = convert_timespec(&in->st_ctim),
  };
}

static cloudabi_errno_t file_stat_fget(cloudabi_fd_t fd,
                                       cloudabi_filestat_t *buf) {
  struct fd_object *fo;
  cloudabi_errno_t error =
      fd_object_get(&fo, fd, CLOUDABI_RIGHT_FILE_STAT_FGET, 0);
  if (error != 0)
    return error;

  int ret;
  switch (fo->type) {
#if !CONFIG_HAS_KQUEUE
    case CLOUDABI_FILETYPE_POLL:
      // TODO(ed): How can we fill in the other fields?
      *buf = (cloudabi_filestat_t){.st_nlink = 1};
      ret = 0;
      break;
#endif
#if !CONFIG_HAS_PDFORK
    case CLOUDABI_FILETYPE_PROCESS:
      // TODO(ed): How can we fill in the other fields?
      *buf = (cloudabi_filestat_t){
          .st_ino = fo->process.pid, .st_nlink = 1,
      };
      ret = 0;
      break;
#endif
    default: {
      struct stat sb;
      ret = fstat(fd_number(fo), &sb);
      convert_stat(&sb, buf);
      break;
    }
  }
  buf->st_filetype = fo->type;
  fd_object_release(fo);
  if (ret < 0)
    return convert_errno(errno);
  return 0;
}

static void convert_timestamp(cloudabi_timestamp_t in, struct timespec *out) {
  static_assert(sizeof(out->tv_sec) >= sizeof(in),
                "Timestamp may get truncated");
  *out = (struct timespec){
      .tv_sec = in / 1000000000, .tv_nsec = in % 1000000000,
  };
}

#if HAS_UTIMENS

// Converts the provided timestamps and flags to a set of arguments for
// futimens() and utimensat().
static void convert_utimens_arguments(const cloudabi_filestat_t *fs,
                                      cloudabi_fsflags_t flags,
                                      struct timespec *ts) {
  if ((flags & CLOUDABI_FILESTAT_ATIM_NOW) != 0) {
    ts[0].tv_nsec = UTIME_NOW;
  } else if ((flags & CLOUDABI_FILESTAT_ATIM) != 0) {
    convert_timestamp(fs->st_atim, &ts[0]);
  } else {
    ts[0].tv_nsec = UTIME_OMIT;
  }

  if ((flags & CLOUDABI_FILESTAT_MTIM_NOW) != 0) {
    ts[1].tv_nsec = UTIME_NOW;
  } else if ((flags & CLOUDABI_FILESTAT_MTIM) != 0) {
    convert_timestamp(fs->st_mtim, &ts[1]);
  } else {
    ts[1].tv_nsec = UTIME_OMIT;
  }
}

#else

// Converts the provided timestamps and flags to a set of arguments for
// futimes(), lutimes() and utimes().
static void convert_utimes_arguments(const cloudabi_filestat_t *fs,
                                     cloudabi_fsflags_t flags,
                                     struct timeval **tv, bool *need_atim,
                                     bool *need_mtim) {
  *need_atim = false;
  *need_mtim = false;
  cloudabi_fsflags_t nowflags =
      flags & (CLOUDABI_FILESTAT_ATIM_NOW | CLOUDABI_FILESTAT_MTIM_NOW);
  if (nowflags == (CLOUDABI_FILESTAT_ATIM_NOW | CLOUDABI_FILESTAT_MTIM_NOW)) {
    // We can invoke futimes() with NULL to update both timestamps.
    *tv = NULL;
  } else {
    struct timeval now;
    if (nowflags != 0)
      gettimeofday(&now, NULL);
    if ((flags & CLOUDABI_FILESTAT_ATIM_NOW) != 0) {
      (*tv)[0] = now;
    } else if ((flags & CLOUDABI_FILESTAT_ATIM) != 0) {
      (*tv)[0].tv_sec = fs->st_atim / 1000000000;
      (*tv)[0].tv_usec = fs->st_atim % 1000000000 / 1000;
    } else {
      *need_atim = true;
    }
    if ((flags & CLOUDABI_FILESTAT_MTIM_NOW) != 0) {
      (*tv)[1] = now;
    } else if ((flags & CLOUDABI_FILESTAT_MTIM) != 0) {
      (*tv)[1].tv_sec = fs->st_mtim / 1000000000;
      (*tv)[1].tv_usec = fs->st_mtim % 1000000000 / 1000;
    } else {
      *need_mtim = true;
    }
  }
}

// Extracts timestamps from a stat structure and places them in the set
// of arguments to be passed to futimes(), lutimes() and utimes().
static void convert_utimes_stat(struct stat *sb, bool need_atim, bool need_mtim,
                                struct timeval *tv) {
  if (need_atim) {
    tv[0].tv_sec = sb->st_atim.tv_sec;
    tv[0].tv_usec = sb->st_atim.tv_nsec / 1000;
  }
  if (need_mtim) {
    tv[1].tv_sec = sb->st_mtim.tv_sec;
    tv[1].tv_usec = sb->st_mtim.tv_nsec / 1000;
  }
}

#endif

static cloudabi_errno_t file_stat_fput(cloudabi_fd_t fd,
                                       const cloudabi_filestat_t *buf,
                                       cloudabi_fsflags_t flags) {
  if ((flags & CLOUDABI_FILESTAT_SIZE) != 0) {
    if ((flags & ~CLOUDABI_FILESTAT_SIZE) != 0)
      return CLOUDABI_EINVAL;

    struct fd_object *fo;
    cloudabi_errno_t error =
        fd_object_get(&fo, fd, CLOUDABI_RIGHT_FILE_STAT_FPUT_SIZE, 0);
    if (error != 0)
      return error;

    int ret = ftruncate(fd_number(fo), buf->st_size);
    fd_object_release(fo);
    if (ret < 0)
      return convert_errno(errno);
    return 0;
#define FLAGS                                            \
  (CLOUDABI_FILESTAT_ATIM | CLOUDABI_FILESTAT_ATIM_NOW | \
   CLOUDABI_FILESTAT_MTIM | CLOUDABI_FILESTAT_MTIM_NOW)
  } else if ((flags & FLAGS) != 0) {
    if ((flags & ~FLAGS) != 0)
      return CLOUDABI_EINVAL;
#undef FLAGS

    struct fd_object *fo;
    cloudabi_errno_t error =
        fd_object_get(&fo, fd, CLOUDABI_RIGHT_FILE_STAT_FPUT_TIMES, 0);
    if (error != 0)
      return error;

#if HAS_UTIMENS
    struct timespec ts[2];
    convert_utimens_arguments(buf, flags, ts);
    int ret = futimens(fd_number(fo), ts);
#else
    struct timeval tv[2];
    struct timeval *tvs = tv;
    bool need_atim, need_mtim;
    convert_utimes_arguments(buf, flags, &tvs, &need_atim, &need_mtim);
    if (need_atim || need_mtim) {
      // We need to preserve some of the original timestamps.
      struct stat sb;
      if (fstat(fd_number(fo), &sb) < 0) {
        fd_object_release(fo);
        return convert_errno(errno);
      }
      convert_utimes_stat(&sb, need_atim, need_mtim, tvs);
    }
    int ret = futimes(fd_number(fo), tvs);
#endif
    fd_object_release(fo);
    if (ret < 0)
      return convert_errno(errno);
    return 0;
  }
  return CLOUDABI_EINVAL;
}

static cloudabi_errno_t file_stat_get(cloudabi_lookup_t fd, const char *path,
                                      size_t pathlen,
                                      cloudabi_filestat_t *buf) {
  struct path_access pa;
  cloudabi_errno_t error =
      path_get(&pa, fd, path, pathlen, CLOUDABI_RIGHT_FILE_STAT_GET, 0, false);
  if (error != 0)
    return error;

  struct stat sb;
  int ret = fstatat(pa.fd, pa.path, &sb, pa.follow ? 0 : AT_SYMLINK_NOFOLLOW);
  path_put(&pa);
  if (ret < 0)
    return convert_errno(errno);
  convert_stat(&sb, buf);

  // Convert the file type. In the case of sockets there is no way we
  // can easily determine the exact socket type.
  if (S_ISBLK(sb.st_mode))
    buf->st_filetype = CLOUDABI_FILETYPE_BLOCK_DEVICE;
  else if (S_ISCHR(sb.st_mode))
    buf->st_filetype = CLOUDABI_FILETYPE_CHARACTER_DEVICE;
  else if (S_ISDIR(sb.st_mode))
    buf->st_filetype = CLOUDABI_FILETYPE_DIRECTORY;
  else if (S_ISFIFO(sb.st_mode))
    buf->st_filetype = CLOUDABI_FILETYPE_FIFO;
  else if (S_ISLNK(sb.st_mode))
    buf->st_filetype = CLOUDABI_FILETYPE_SYMBOLIC_LINK;
  else if (S_ISREG(sb.st_mode))
    buf->st_filetype = CLOUDABI_FILETYPE_REGULAR_FILE;
  else if (S_ISSOCK(sb.st_mode))
    buf->st_filetype = CLOUDABI_FILETYPE_SOCKET_STREAM;
  return 0;
}

static cloudabi_errno_t file_stat_put(cloudabi_lookup_t fd, const char *path,
                                      size_t pathlen,
                                      const cloudabi_filestat_t *buf,
                                      cloudabi_fsflags_t flags) {
  if ((flags &
       ~(CLOUDABI_FILESTAT_ATIM | CLOUDABI_FILESTAT_ATIM_NOW |
         CLOUDABI_FILESTAT_MTIM | CLOUDABI_FILESTAT_MTIM_NOW)) != 0)
    return CLOUDABI_EINVAL;

  struct path_access pa;
  cloudabi_errno_t error = path_get(
      &pa, fd, path, pathlen, CLOUDABI_RIGHT_FILE_STAT_PUT_TIMES, 0, false);
  if (error != 0)
    return error;

#if HAS_UTIMENS
  struct timespec ts[2];
  convert_utimens_arguments(buf, flags, ts);
  int ret = utimensat(pa.fd, pa.path, ts, pa.follow ? 0 : AT_SYMLINK_NOFOLLOW);
#else
  struct timeval tv[2];
  struct timeval *tvs = tv;
  bool need_atim, need_mtim;
  convert_utimes_arguments(buf, flags, &tvs, &need_atim, &need_mtim);
  if (need_atim || need_mtim) {
    // We need to preserve some of the original timestamps.
    struct stat sb;
    if (fstatat(pa.fd, pa.path, &sb, pa.follow ? 0 : AT_SYMLINK_NOFOLLOW) < 0) {
      path_put(&pa);
      return convert_errno(errno);
    }
    convert_utimes_stat(&sb, need_atim, need_mtim, tvs);
  }
  error = cwd_get(&pa);
  if (error != 0) {
    path_put(&pa);
    return error;
  }
  int ret = (pa.follow ? utimes : lutimes)(pa.path, tvs);
  cwd_put();
#endif
  path_put(&pa);
  if (ret < 0)
    return convert_errno(errno);
  return 0;
}

static cloudabi_errno_t file_symlink(const char *path1, size_t path1len,
                                     cloudabi_fd_t fd, const char *path2,
                                     size_t path2len) {
  char *target = str_nullterminate(path1, path1len);
  if (target == NULL)
    return convert_errno(errno);

  struct path_access pa;
  cloudabi_errno_t error = path_get_nofollow(
      &pa, fd, path2, path2len, CLOUDABI_RIGHT_FILE_SYMLINK, 0, true);
  if (error != 0) {
    free(target);
    return error;
  }

  int ret = symlinkat(target, pa.fd, pa.path);
  path_put(&pa);
  free(target);
  if (ret < 0)
    return convert_errno(errno);
  return 0;
}

static cloudabi_errno_t file_unlink(cloudabi_fd_t fd, const char *path,
                                    size_t pathlen, cloudabi_ulflags_t flags) {
  struct path_access pa;
  cloudabi_errno_t error = path_get_nofollow(
      &pa, fd, path, pathlen, CLOUDABI_RIGHT_FILE_UNLINK, 0, true);
  if (error != 0)
    return error;

  int ret =
      unlinkat(pa.fd, pa.path,
               (flags & CLOUDABI_UNLINK_REMOVEDIR) != 0 ? AT_REMOVEDIR : 0);
  path_put(&pa);
  if (ret < 0) {
    // Linux returns EISDIR, whereas EPERM is what's required by POSIX.
    return errno == EISDIR ? CLOUDABI_EPERM : convert_errno(errno);
  }
  return 0;
}

static cloudabi_errno_t lock_unlock(_Atomic(cloudabi_lock_t) * lock,
                                    cloudabi_scope_t scope) {
  return futex_op_lock_unlock(curtid, lock, scope);
}

static cloudabi_errno_t mem_advise(void *addr, size_t len,
                                   cloudabi_advice_t advice) {
  int nadvice;
  switch (advice) {
    case CLOUDABI_ADVICE_DONTNEED:
      nadvice = POSIX_MADV_DONTNEED;
      break;
    case CLOUDABI_ADVICE_NORMAL:
      nadvice = POSIX_MADV_NORMAL;
      break;
    case CLOUDABI_ADVICE_RANDOM:
      nadvice = POSIX_MADV_RANDOM;
      break;
    case CLOUDABI_ADVICE_SEQUENTIAL:
      nadvice = POSIX_MADV_SEQUENTIAL;
      break;
    case CLOUDABI_ADVICE_WILLNEED:
      nadvice = POSIX_MADV_WILLNEED;
      break;
    default:
      return CLOUDABI_EINVAL;
  }

  int error = posix_madvise(addr, len, nadvice);
  if (error != 0)
    return convert_errno(error);
  return 0;
}

static cloudabi_errno_t mem_lock(const void *addr, size_t len) {
  if (mlock(addr, len) < 0)
    return convert_errno(errno);
  return 0;
}

static bool convert_mprot(cloudabi_mflags_t in, int *out) {
  // Test for invalid bits.
  if ((in & ~(CLOUDABI_PROT_READ | CLOUDABI_PROT_WRITE | CLOUDABI_PROT_EXEC)) !=
      0)
    return false;

  // Don't allow PROT_WRITE and PROT_EXEC at the same time.
  if ((in & CLOUDABI_PROT_WRITE) != 0 && (in & CLOUDABI_PROT_EXEC) != 0)
    return false;

  *out = 0;
  if (in & CLOUDABI_PROT_READ)
    *out |= PROT_READ;
  if (in & CLOUDABI_PROT_WRITE)
    *out |= PROT_WRITE;
  if (in & CLOUDABI_PROT_EXEC)
    *out |= PROT_EXEC;
  return true;
}

static cloudabi_errno_t mem_map(void *addr, size_t len, cloudabi_mprot_t prot,
                                cloudabi_mflags_t flags, cloudabi_fd_t fd,
                                cloudabi_filesize_t off, void **mem) {
  int nprot;
  if (!convert_mprot(prot, &nprot))
    return CLOUDABI_ENOTSUP;

  int nflags = 0;
  if ((flags & CLOUDABI_MAP_FIXED) != 0)
    nflags |= MAP_FIXED;
  switch (flags & (CLOUDABI_MAP_PRIVATE | CLOUDABI_MAP_SHARED)) {
    case CLOUDABI_MAP_PRIVATE:
      nflags |= MAP_PRIVATE;
      break;
    case CLOUDABI_MAP_SHARED:
      nflags |= MAP_SHARED;
      break;
    default:
      return CLOUDABI_EINVAL;
  }

  void *ret;
  if ((flags & CLOUDABI_MAP_ANON) != 0) {
    // Mapping anonymous memory.
    if (fd != CLOUDABI_MAP_ANON_FD || off != 0)
      return CLOUDABI_EINVAL;
    nflags |= MAP_ANON;
    ret = mmap(addr, len, nprot, nflags, -1, 0);
  } else {
    // Mapping backed by a file.
    struct fd_object *fo;
    // TODO(ed): Determine rights!
    cloudabi_errno_t error = fd_object_get(&fo, fd, 0, 0);
    if (error != 0)
      return error;
    ret = mmap(addr, len, nprot, nflags, fd_number(fo), off);
    fd_object_release(fo);
  }
  if (ret == MAP_FAILED)
    return convert_errno(errno);
  *mem = ret;
  return 0;
}

static cloudabi_errno_t mem_protect(void *addr, size_t len,
                                    cloudabi_mprot_t prot) {
  int nprot;
  if (!convert_mprot(prot, &nprot))
    return CLOUDABI_ENOTSUP;
  if (mprotect(addr, len, nprot) < 0)
    return convert_errno(errno);
  return 0;
}

static cloudabi_errno_t mem_sync(void *addr, size_t len,
                                 cloudabi_msflags_t flags) {
  int nflags = 0;
  switch (flags & (CLOUDABI_MS_ASYNC | CLOUDABI_MS_SYNC)) {
    case CLOUDABI_MS_ASYNC:
      nflags |= MS_ASYNC;
      break;
    case CLOUDABI_MS_SYNC:
      nflags |= MS_SYNC;
      break;
    default:
      return CLOUDABI_EINVAL;
  }
  if ((flags & CLOUDABI_MS_INVALIDATE) != 0)
    nflags |= MS_INVALIDATE;

  if (msync(addr, len, flags) < 0)
    return convert_errno(errno);
  return 0;
}

static cloudabi_errno_t mem_unlock(const void *addr, size_t len) {
  if (munlock(addr, len) < 0)
    return convert_errno(errno);
  return 0;
}

static cloudabi_errno_t mem_unmap(void *addr, size_t len) {
  if (munmap(addr, len) < 0)
    return convert_errno(errno);
  return 0;
}

// Converts a POSIX signal number to a CloudABI signal number.
static cloudabi_signal_t convert_signal(int sig) {
  static const cloudabi_signal_t signals[] = {
#define X(v) [v] = CLOUDABI_##v
      X(SIGABRT), X(SIGALRM), X(SIGBUS), X(SIGCHLD), X(SIGCONT), X(SIGFPE),
      X(SIGHUP),  X(SIGILL),  X(SIGINT), X(SIGKILL), X(SIGPIPE), X(SIGQUIT),
      X(SIGSEGV), X(SIGSTOP), X(SIGSYS), X(SIGTERM), X(SIGTRAP), X(SIGTSTP),
      X(SIGTTIN), X(SIGTTOU), X(SIGURG), X(SIGUSR1), X(SIGUSR2), X(SIGVTALRM),
      X(SIGXCPU), X(SIGXFSZ),
#undef X
  };
  if (sig < 0 || sig >= sizeof(signals) / sizeof(signals[0]) ||
      signals[sig] == 0)
    return CLOUDABI_SIGABRT;
  return signals[sig];
}

static cloudabi_errno_t do_pdwait(cloudabi_fd_t fd, bool nohang,
                                  cloudabi_signal_t *signal,
                                  cloudabi_exitcode_t *exitcode) {
  struct fd_object *fo;
  cloudabi_errno_t error =
      fd_object_get(&fo, fd, CLOUDABI_RIGHT_POLL_PROC_TERMINATE, 0);
  if (error != 0)
    return error;

#if CONFIG_HAS_PDFORK
  siginfo_t si;
  int ret = pdwait(fd_number(fo), &si, nohang ? WNOHANG : 0);
  if (ret != 0 || si.si_signo != SIGCHLD) {
    // Error or still running.
    fd_object_release(fo);
    return ret == 0 ? CLOUDABI_EAGAIN : convert_errno(ret);
  }
  if (si.si_code == CLD_EXITED) {
    // Process has exited.
    *signal = 0;
    *exitcode = si.si_status;
  } else {
    // Process has terminated because of a signal.
    *signal = convert_signal(si.si_status);
    *exitcode = 0;
  }
#else
  // Wait on the process if termination info is not available yet.
  mutex_lock(&fo->process.lock);
  if (!fo->process.terminated) {
    int pstat;
    int ret = waitpid(fo->process.pid, &pstat, nohang ? WNOHANG : 0);
    if (ret <= 0) {
      // Error or still running.
      mutex_unlock(&fo->process.lock);
      fd_object_release(fo);
      return ret == 0 ? CLOUDABI_EAGAIN : convert_errno(errno);
    }
    if (WIFEXITED(pstat)) {
      // Process has exited.
      fo->process.signal = 0;
      fo->process.exitcode = WEXITSTATUS(pstat);
    } else {
      // Process has terminated because of a signal.
      fo->process.signal = convert_signal(WTERMSIG(pstat));
      fo->process.exitcode = 0;
    }
    fo->process.terminated = true;
  }
  *signal = fo->process.signal;
  *exitcode = fo->process.exitcode;
  mutex_unlock(&fo->process.lock);
#endif

  fd_object_release(fo);
  return 0;
}

static cloudabi_errno_t poll(const cloudabi_subscription_t *in,
                             cloudabi_event_t *out, size_t nsubscriptions,
                             size_t *nevents) {
  // Capture poll() calls that deal with futexes.
  if (futex_op_poll(curtid, in, out, nsubscriptions, nevents))
    return 0;

  // Sleeping.
  if (nsubscriptions == 1 && in[0].type == CLOUDABI_EVENTTYPE_CLOCK) {
    out[0] = (cloudabi_event_t){
        .userdata = in[0].userdata,
        .type = in[0].type,
        .clock.identifier = in[0].clock.identifier,
    };
#if CONFIG_HAS_CLOCK_NANOSLEEP
    clockid_t clock_id;
    if (convert_clockid(in[0].clock.clock_id, &clock_id)) {
      struct timespec ts;
      convert_timestamp(in[0].clock.timeout, &ts);
      int ret = clock_nanosleep(
          clock_id,
          (in[0].clock.flags & CLOUDABI_SUBSCRIPTION_CLOCK_ABSTIME) != 0
              ? TIMER_ABSTIME
              : 0,
          &ts, NULL);
      if (ret != 0)
        out[0].error = convert_errno(ret);
    } else {
      out[0].error = CLOUDABI_ENOTSUP;
    }
#else
    switch (in[0].clock.clock_id) {
      case CLOUDABI_CLOCK_MONOTONIC:
        if ((in[0].clock.flags & CLOUDABI_SUBSCRIPTION_CLOCK_ABSTIME) != 0) {
          // TODO(ed): Implement.
          fprintf(stderr, "Unimplemented absolute sleep on monotonic clock\n");
          out[0].error = CLOUDABI_ENOSYS;
        } else {
          // Perform relative sleeps on the monotonic clock also using
          // nanosleep(). This is incorrect, but good enough for now.
          struct timespec ts;
          convert_timestamp(in[0].clock.timeout, &ts);
          nanosleep(&ts, NULL);
        }
        break;
      case CLOUDABI_CLOCK_REALTIME:
        if ((in[0].clock.flags & CLOUDABI_SUBSCRIPTION_CLOCK_ABSTIME) != 0) {
          // Sleeping to an absolute point in time can only be done
          // by waiting on a condition variable.
          struct mutex mutex;
          mutex_init(&mutex);
          struct cond cond;
          cond_init(&cond);
          mutex_lock(&mutex);
          cond_timedwait(&cond, &mutex, in[0].clock.timeout);
          mutex_unlock(&mutex);
          mutex_destroy(&mutex);
          cond_destroy(&cond);
        } else {
          // Relative sleeps can be done using nanosleep().
          struct timespec ts;
          convert_timestamp(in[0].clock.timeout, &ts);
          nanosleep(&ts, NULL);
        }
        break;
      default:
        out[0].error = CLOUDABI_ENOTSUP;
        break;
    }
#endif
    return 0;
  }

  // Process event used by pdwait(): waiting on a process descriptor.
  if ((nsubscriptions == 1 &&
       in[0].type == CLOUDABI_EVENTTYPE_PROC_TERMINATE) ||
      (nsubscriptions == 2 && in[0].type == CLOUDABI_EVENTTYPE_PROC_TERMINATE &&
       in[1].type == CLOUDABI_EVENTTYPE_CLOCK && in[1].clock.timeout == 0)) {
    out[0] = (cloudabi_event_t){
        .userdata = in[0].userdata,
        .type = in[0].type,
        .proc_terminate.fd = in[0].proc_terminate.fd,
    };
    out[0].error = do_pdwait(in[0].proc_terminate.fd, nsubscriptions == 2,
                             &out[0].proc_terminate.signal,
                             &out[0].proc_terminate.exitcode);
    if (out[0].error == CLOUDABI_EAGAIN) {
      // Child process has not terminated yet. Return the clock event
      // instead.
      out[0] = (cloudabi_event_t){
          .userdata = in[1].userdata,
          .type = in[1].type,
          .clock.identifier = in[1].clock.identifier,
          .error = 0,
      };
    }
    *nevents = 1;
    return 0;
  }

  fprintf(stderr, "Unimplemented poll()\n");
  return CLOUDABI_ENOSYS;
}

static cloudabi_errno_t poll_fd(cloudabi_fd_t fd,
                                const cloudabi_subscription_t *in, size_t nin,
                                cloudabi_event_t *out, size_t nout,
                                const cloudabi_subscription_t *timeout,
                                size_t *nevents) {
  fprintf(stderr, "Unimplemented poll_fd()\n");
  return CLOUDABI_ENOSYS;
}

static cloudabi_errno_t proc_exec(cloudabi_fd_t fd, const void *data,
                                  size_t datalen, const cloudabi_fd_t *fds,
                                  size_t fdslen) {
  fprintf(stderr, "Unimplemented proc_exec()\n");
  return CLOUDABI_ENOSYS;
}

static void proc_exit(cloudabi_exitcode_t rval) {
  _Exit(rval);
}

static cloudabi_errno_t proc_fork(cloudabi_fd_t *fd, cloudabi_tid_t *tid) {
  // Lock down the file descriptor table while forking, to ensure it's
  // consistent after forking.
  struct fd_table *ft = curfds;
  rwlock_wrlock(&ft->lock);
#if CONFIG_HAS_PDFORK
  int nfd;
  int pid = pdfork(&nfd, 0);
#else
  pid_t pid = fork();
#endif
  rwlock_unlock(&ft->lock);
  if (pid < 0)
    return convert_errno(errno);

  if (pid == 0) {
#if HAS_CWD_LOCK
    // Inside the child process.
    mutex_init(&cwd_lock);
#endif
    *fd = CLOUDABI_PROCESS_CHILD;
    tidpool_postfork();
    futex_postfork();
    *tid = tidpool_allocate();
    return 0;
  } else {
#if CONFIG_HAS_PDFORK
    // Inside the parent process.
    return fd_table_insert_fd(ft, nfd, CLOUDABI_FILETYPE_PROCESS,
                              RIGHTS_PROCESS_BASE, RIGHTS_PROCESS_INHERITING,
                              fd);
#else
    struct fd_object *fo;
    cloudabi_errno_t error = fd_object_new(CLOUDABI_FILETYPE_PROCESS, &fo);
    if (error != 0) {
      kill(pid, SIGSEGV);
      waitpid(pid, NULL, 0);
      return error;
    }
    mutex_init(&fo->process.lock);
    fo->process.terminated = false;
    fo->process.pid = pid;
    return fd_table_insert(ft, fo, RIGHTS_PROCESS_BASE,
                           RIGHTS_PROCESS_INHERITING, fd);
#endif
  }
}

static cloudabi_errno_t proc_raise(cloudabi_signal_t sig) {
  static const int signals[] = {
#define X(v) [CLOUDABI_##v] = v
      X(SIGABRT), X(SIGALRM), X(SIGBUS), X(SIGCHLD), X(SIGCONT), X(SIGFPE),
      X(SIGHUP),  X(SIGILL),  X(SIGINT), X(SIGKILL), X(SIGPIPE), X(SIGQUIT),
      X(SIGSEGV), X(SIGSTOP), X(SIGSYS), X(SIGTERM), X(SIGTRAP), X(SIGTSTP),
      X(SIGTTIN), X(SIGTTOU), X(SIGURG), X(SIGUSR1), X(SIGUSR2), X(SIGVTALRM),
      X(SIGXCPU), X(SIGXFSZ),
#undef X
  };
  if (sig >= sizeof(signals) / sizeof(signals[0]) || signals[sig] == 0)
    return CLOUDABI_EINVAL;

#if CONFIG_TLS_USE_GSBASE
  // TLS on OS X depends on installing a SIGSEGV handler. Reset SIGSEGV
  // to the default action before raising.
  if (sig == CLOUDABI_SIGSEGV) {
    struct sigaction sa = {
        .sa_handler = SIG_DFL,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
  }
#endif

  if (raise(signals[sig]) < 0)
    return convert_errno(errno);
  return 0;
}

static cloudabi_errno_t random_get(void *buf, size_t nbyte) {
  random_buf(buf, nbyte);
  return 0;
}

// Converts a socket address structure to a CloudABI socket address.
static void convert_sockaddr(const struct sockaddr_storage *sa, socklen_t sal,
                             cloudabi_sockaddr_t *rsa) {
  // Zero-sized socket address.
  if (sal <
      offsetof(struct sockaddr_storage, ss_family) + sizeof(sa->ss_family))
    return;

  switch (sa->ss_family) {
    case AF_INET: {
      if (sal < sizeof(struct sockaddr_in))
        return;
      const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
      rsa->sa_family = CLOUDABI_AF_INET;
      memcpy(&rsa->sa_inet.addr, &sin->sin_addr, sizeof(rsa->sa_inet.addr));
      rsa->sa_inet.port = ntohs(sin->sin_port);
      return;
    }
    case AF_INET6:
      if (sal < sizeof(struct sockaddr_in6))
        return;
      const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
      rsa->sa_family = CLOUDABI_AF_INET6;
      memcpy(&rsa->sa_inet6.addr, &sin6->sin6_addr, sizeof(rsa->sa_inet6.addr));
      rsa->sa_inet6.port = ntohs(sin6->sin6_port);
      return;
    case AF_UNIX:
      rsa->sa_family = CLOUDABI_AF_UNIX;
      return;
  }
}

static cloudabi_errno_t sock_accept(cloudabi_fd_t sock,
                                    cloudabi_sockstat_t *buf,
                                    cloudabi_fd_t *conn) {
  // Fetch socket file descriptor and rights.
  struct fd_table *ft = curfds;
  rwlock_wrlock(&ft->lock);
  struct fd_entry *fe;
  cloudabi_errno_t error =
      fd_table_get_entry(ft, sock, CLOUDABI_RIGHT_SOCK_ACCEPT, 0, &fe);
  if (error != 0) {
    rwlock_unlock(&ft->lock);
    return error;
  }
  cloudabi_rights_t rights = fe->rights_inheriting;
  struct fd_object *fo = fe->object;
  refcount_acquire(&fo->refcount);
  rwlock_unlock(&ft->lock);
  cloudabi_filetype_t type = fo->type;

  int nfd;
  if (buf == NULL) {
    // No peer address requested.
    nfd = accept(fd_number(fo), NULL, NULL);
    fd_object_release(fo);
  } else {
    // Peer address requested.
    struct sockaddr_storage ss;
    socklen_t sslen = sizeof(ss);
    nfd = accept(fd_number(fo), (struct sockaddr *)&ss, &sslen);
    fd_object_release(fo);

    // TODO(ed): How to fill in the other members?
    *buf = (cloudabi_sockstat_t){};
    convert_sockaddr(&ss, sslen, &buf->ss_peername);
  }
  if (nfd < 0)
    return convert_errno(errno);
  return fd_table_insert_fd(curfds, nfd, type, RIGHTS_SOCKET_BASE & rights,
                            RIGHTS_SOCKET_INHERITING & rights, conn);
}

static cloudabi_errno_t sock_bind(cloudabi_fd_t sock, cloudabi_fd_t fd,
                                  const char *path, size_t pathlen) {
  struct fd_object *fo;
  cloudabi_errno_t error =
      fd_object_get(&fo, sock, CLOUDABI_RIGHT_SOCK_BIND_SOCKET, 0);
  if (error != 0)
    return error;

  struct path_access pa;
  error = path_get_nofollow(&pa, fd, path, pathlen,
                            CLOUDABI_RIGHT_SOCK_BIND_DIRECTORY, 0, true);
  if (error != 0) {
    fd_object_release(fo);
    return error;
  }

#if CONFIG_HAS_BINDAT_STRING
  int ret = bindat(fd_number(fo), pa.fd, pa.path);
#else
  struct sockaddr_un sun;
  sun.sun_family = AF_UNIX;
  if (strlcpy(sun.sun_path, pa.path, sizeof(sun.sun_path)) >=
      sizeof(sun.sun_path)) {
    fd_object_release(fo);
    path_put(&pa);
    return CLOUDABI_ENAMETOOLONG;
  }
#if CONFIG_HAS_BINDAT_SOCKADDR
  int ret = bindat(pa.fd, fd_number(fo), (struct sockaddr *)&sun, sizeof(sun));
#else
  error = cwd_get(&pa);
  if (error != 0) {
    fd_object_release(fo);
    path_put(&pa);
    return error;
  }
  int ret = bind(fd_number(fo), (struct sockaddr *)&sun, sizeof(sun));
  cwd_put();
#endif
#endif
  fd_object_release(fo);
  path_put(&pa);
  if (ret < 0)
    return convert_errno(errno);
  return 0;
}

static cloudabi_errno_t sock_connect(cloudabi_fd_t sock, cloudabi_fd_t fd,
                                     const char *path, size_t pathlen) {
  struct fd_object *fo;
  cloudabi_errno_t error =
      fd_object_get(&fo, sock, CLOUDABI_RIGHT_SOCK_CONNECT_SOCKET, 0);
  if (error != 0)
    return error;

  struct path_access pa;
  error = path_get_nofollow(&pa, fd, path, pathlen,
                            CLOUDABI_RIGHT_SOCK_CONNECT_DIRECTORY, 0, false);
  if (error != 0) {
    fd_object_release(fo);
    return error;
  }

#if CONFIG_HAS_CONNECTAT_STRING
  int ret = connectat(fd_number(fo), pa.fd, pa.path);
#else
  struct sockaddr_un sun;
  sun.sun_family = AF_UNIX;
  if (strlcpy(sun.sun_path, pa.path, sizeof(sun.sun_path)) >=
      sizeof(sun.sun_path)) {
    fd_object_release(fo);
    path_put(&pa);
    return CLOUDABI_ENAMETOOLONG;
  }
#if CONFIG_HAS_CONNECTAT_SOCKADDR
  int ret =
      connectat(pa.fd, fd_number(fo), (struct sockaddr *)&sun, sizeof(sun));
#else
  error = cwd_get(&pa);
  if (error != 0) {
    fd_object_release(fo);
    path_put(&pa);
    return error;
  }
  int ret = connect(fd_number(fo), (struct sockaddr *)&sun, sizeof(sun));
  cwd_put();
#endif
#endif
  fd_object_release(fo);
  path_put(&pa);
  if (ret < 0)
    return convert_errno(errno);
  return 0;
}

static cloudabi_errno_t sock_listen(cloudabi_fd_t sock,
                                    cloudabi_backlog_t backlog) {
  struct fd_object *fo;
  cloudabi_errno_t error =
      fd_object_get(&fo, sock, CLOUDABI_RIGHT_SOCK_LISTEN, 0);
  if (error != 0)
    return error;

  int ret = listen(fd_number(fo), backlog);
  if (ret < 0) {
    if (errno == EINVAL) {
      // Common bug in the BSDs: listen() returns EINVAL both when not
      // bound and when already connected. Determine whether to return
      // EINVAL or EDESTADDRREQ.
      struct sockaddr_storage ss;
      socklen_t sslen = sizeof(ss);
      bool connected =
          getpeername(fd_number(fo), (struct sockaddr *)&ss, &sslen) == 0 ||
          errno != ENOTCONN;
      fd_object_release(fo);
      return connected ? CLOUDABI_EINVAL : CLOUDABI_EDESTADDRREQ;
    } else {
      fd_object_release(fo);
      return convert_errno(errno);
    }
  } else {
    fd_object_release(fo);
    return 0;
  }
}

static cloudabi_errno_t sock_recv(cloudabi_fd_t sock,
                                  const cloudabi_recv_in_t *in,
                                  cloudabi_recv_out_t *out) {
  // Convert input to msghdr.
  struct sockaddr_storage ss;
  struct msghdr hdr = {
      .msg_name = &ss,
      .msg_namelen = sizeof(ss),
      .msg_iov = (struct iovec *)in->ri_data,
      .msg_iovlen = in->ri_datalen,
  };
  int nflags = 0;
  if ((in->ri_flags & CLOUDABI_MSG_PEEK) != 0)
    nflags |= MSG_PEEK;
  if ((in->ri_flags & CLOUDABI_MSG_WAITALL) != 0)
    nflags |= MSG_WAITALL;

  // TODO(ed): Support receiving file descriptors!

  struct fd_object *fo;
  cloudabi_errno_t error = fd_object_get(&fo, sock, CLOUDABI_RIGHT_FD_READ, 0);
  if (error != 0)
    return error;

  ssize_t len = recvmsg(fd_number(fo), &hdr, nflags);
  fd_object_release(fo);
  if (len < 0)
    return convert_errno(errno);

  // Convert msghdr to output.
  *out = (cloudabi_recv_out_t){
      .ro_datalen = len,
  };
  convert_sockaddr(&ss, hdr.msg_namelen, &out->ro_peername);
  if ((hdr.msg_flags & MSG_CTRUNC) != 0)
    out->ro_flags |= CLOUDABI_MSG_CTRUNC;
  if ((hdr.msg_flags & MSG_EOR) != 0)
    out->ro_flags |= CLOUDABI_MSG_EOR;
  if ((hdr.msg_flags & MSG_TRUNC) != 0)
    out->ro_flags |= CLOUDABI_MSG_TRUNC;
  return 0;
}

static cloudabi_errno_t sock_send(cloudabi_fd_t sock,
                                  const cloudabi_send_in_t *in,
                                  cloudabi_send_out_t *out) {
  // Convert input to msghdr.
  struct msghdr hdr = {
      .msg_iov = (struct iovec *)in->si_data, .msg_iovlen = in->si_datalen,
  };
  int nflags = 0;
  if ((in->si_flags & CLOUDABI_MSG_EOR) != 0)
    nflags |= MSG_EOR;

  // TODO(ed): Support sending file descriptors!

  struct fd_object *fo;
  cloudabi_errno_t error = fd_object_get(&fo, sock, CLOUDABI_RIGHT_FD_WRITE, 0);
  if (error != 0)
    return error;

  ssize_t len = sendmsg(fd_number(fo), &hdr, nflags);
  fd_object_release(fo);
  if (len < 0)
    return convert_errno(errno);

  // Convert msghdr to output.
  *out = (cloudabi_send_out_t){
      .so_datalen = len,
  };
  return 0;
}

static cloudabi_errno_t sock_shutdown(cloudabi_fd_t sock,
                                      cloudabi_sdflags_t how) {
  int nhow;
  switch (how) {
    case CLOUDABI_SHUT_RD:
      nhow = SHUT_RD;
      break;
    case CLOUDABI_SHUT_WR:
      nhow = SHUT_WR;
      break;
    case CLOUDABI_SHUT_RD | CLOUDABI_SHUT_WR:
      nhow = SHUT_RDWR;
      break;
    default:
      return CLOUDABI_EINVAL;
  }

  struct fd_object *fo;
  cloudabi_errno_t error =
      fd_object_get(&fo, sock, CLOUDABI_RIGHT_SOCK_SHUTDOWN, 0);
  if (error != 0)
    return error;

  int ret = shutdown(fd_number(fo), nhow);
  fd_object_release(fo);
  if (ret < 0)
    return convert_errno(errno);
  return 0;
}

static cloudabi_errno_t sock_stat_get(cloudabi_fd_t sock,
                                      cloudabi_sockstat_t *buf,
                                      cloudabi_ssflags_t flags) {
  struct fd_object *fo;
  cloudabi_errno_t error =
      fd_object_get(&fo, sock, CLOUDABI_RIGHT_SOCK_STAT_GET, 0);
  if (error != 0)
    return error;

  // Fetch and convert socket and peer addresses.
  *buf = (cloudabi_sockstat_t){};
  struct sockaddr_storage ss;
  socklen_t sslen = sizeof(ss);
  if (getsockname(fd_number(fo), (struct sockaddr *)&ss, &sslen) == 0)
    convert_sockaddr(&ss, sslen, &buf->ss_sockname);
  sslen = sizeof(ss);
  if (getpeername(fd_number(fo), (struct sockaddr *)&ss, &sslen) == 0)
    convert_sockaddr(&ss, sslen, &buf->ss_peername);

  // Fill ss_error.
  if ((flags & CLOUDABI_SOCKSTAT_CLEAR_ERROR) != 0) {
    int error;
    socklen_t errorlen = sizeof(error);
    if (getsockopt(fd_number(fo), SOL_SOCKET, SO_ERROR, &error, &errorlen) ==
        -1) {
      fd_object_release(fo);
      return convert_errno(errno);
    }
    if (error != 0)
      buf->ss_error = convert_errno(error);
  }

  // Fill ss_state.
  int acceptconn;
  socklen_t acceptconnlen = sizeof(acceptconn);
  if (getsockopt(fd_number(fo), SOL_SOCKET, SO_ACCEPTCONN, &acceptconn,
                 &acceptconnlen) == -1) {
    if (errno != ENOPROTOOPT) {
      fd_object_release(fo);
      return convert_errno(errno);
    }
  } else if (acceptconn) {
    buf->ss_state |= CLOUDABI_SOCKSTATE_ACCEPTCONN;
  }

  fd_object_release(fo);
  return 0;
}

struct thread_params {
  cloudabi_threadentry_t *entry_point;
  cloudabi_tid_t tid;
  void *argument;
  struct fd_table *fd_table;
};

static void *thread_entry(void *thunk) {
  struct thread_params params = *(struct thread_params *)thunk;
  free(thunk);

  curfds = params.fd_table;
  curtid = params.tid;
  struct tls tls;
  tls_init(&tls, &posix_syscalls);

  // Pass on execution to the thread's entry point. It should never
  // return, but call thread_exit() instead.
  params.entry_point(params.tid, params.argument);
  abort();
}

static cloudabi_errno_t thread_create(cloudabi_threadattr_t *attr,
                                      cloudabi_tid_t *tid) {
  // Create parameters that need to be passed on to the thread.
  // thread_entry() is responsible for freeing it again.
  struct thread_params *params = malloc(sizeof(*params));
  if (params == NULL)
    return CLOUDABI_ENOMEM;
  params->entry_point = attr->entry_point;
  params->tid = *tid = tidpool_allocate();
  params->argument = attr->argument;
  params->fd_table = curfds;

  pthread_attr_t nattr;
  int ret = pthread_attr_init(&nattr);
  if (ret != 0) {
    free(params);
    return convert_errno(ret);
  }

  // Make the thread detached, because we're not going to join on it.
  ret = pthread_attr_setdetachstate(&nattr, PTHREAD_CREATE_DETACHED);
  if (ret != 0) {
    free(params);
    pthread_attr_destroy(&nattr);
    return convert_errno(ret);
  }

  // Allocate a stack with the same size, but do not use the buffer
  // provided by the application. The stack of the executable is also
  // used by the emulator. The wakeup performed by thread_exit() may
  // cause another thread in the application to free the stack while
  // we're still shutting down.
  pthread_attr_setstacksize(&nattr, attr->stack_size);

  // Spawn a new thread.
  pthread_t thread;
  ret = pthread_create(&thread, &nattr, thread_entry, params);
  pthread_attr_destroy(&nattr);
  if (ret != 0) {
    free(params);
    return convert_errno(ret);
  }
  return 0;
}

static void thread_exit(_Atomic(cloudabi_lock_t) * lock,
                        cloudabi_scope_t scope) {
  // Drop the lock, so threads waiting to join this thread get woken up.
  futex_op_lock_unlock(curtid, lock, scope);

  // Terminate the execution of this thread.
  pthread_exit(NULL);
}

static cloudabi_errno_t thread_tcb_set(void *tcb) {
  // This system call is deprecated and cannot be emulated safely.
  return CLOUDABI_ENOSYS;
}

static cloudabi_errno_t thread_yield(void) {
  if (sched_yield() < 0)
    return convert_errno(errno);
  return 0;
}

cloudabi_syscalls_t posix_syscalls = {
#define entry(name) .name = name,
    CLOUDABI_SYSCALL_NAMES(entry)
#undef entry
};
