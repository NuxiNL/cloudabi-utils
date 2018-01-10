// Copyright (c) 2016-2018 Nuxi, https://nuxi.nl/
//
// SPDX-License-Identifier: BSD-2-Clause

#include "config.h"

#if CONFIG_HAS_CAP_ENTER
#include <sys/capsicum.h>
#endif
#include <sys/mman.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cloudabi_syscalls_info.h>
#include <cloudabi_types.h>

#include "elf.h"
#include "emulate.h"
#include "posix.h"
#include "random.h"
#include "signals.h"
#include "tidpool.h"
#include "tls.h"

#ifndef roundup
#define roundup(a, b) (((a) + (b)-1) / (b) * (b))
#endif

// Reads data from a file at an offset and validates that the requested
// amount of data is returned.
static bool do_pread(int fd, void *buf, size_t len, off_t pos) {
  char *bufp = buf;
  while (len > 0) {
    ssize_t retval = pread(fd, bufp, len, pos);
    if (retval < 0)
      return false;
    if (retval == 0) {
      // Short reads should not occur.
      errno = ENOEXEC;
      return false;
    }
    bufp += retval;
    pos += retval;
    len -= retval;
  }
  return true;
}

void emulate(int fd, const void *argdata, size_t argdatalen,
             const struct syscalls *syscalls) {
  // Parse the ELF header.
  ElfW(Ehdr) ehdr;
  if (!do_pread(fd, &ehdr, sizeof(ehdr), 0))
    return;

  // Validate that this is a CloudABI executable for the current
  // architecture.
  if (ehdr.e_ident[EI_MAG0] != ELFMAG0 || ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
      ehdr.e_ident[EI_MAG2] != ELFMAG2 || ehdr.e_ident[EI_MAG3] != ELFMAG3 ||
      ehdr.e_ident[EI_OSABI] != ELFOSABI_CLOUDABI || ehdr.e_type != ET_DYN ||
#if defined(__aarch64__)
      ehdr.e_machine != EM_AARCH64
#elif defined(__arm__)
      ehdr.e_machine != EM_ARM
#elif defined(__i386__)
      ehdr.e_machine != EM_386
#elif defined(__x86_64__)
      ehdr.e_machine != EM_X86_64
#else
#error "Unsupported architecture"
#endif
  ) {
    errno = ENOEXEC;
    return;
  }

  // Copy in the program headers.
  ElfW(Phdr) phdrs[ehdr.e_phnum];
  if (!do_pread(fd, phdrs, sizeof(ElfW(Phdr)) * ehdr.e_phnum, ehdr.e_phoff))
    return;

  // Scan over the program headers to determine the virtual address
  // region at which this program is mapped.
  size_t addr_begin = SIZE_MAX;
  size_t addr_end = 0;
  size_t pagesize = sysconf(_SC_PAGESIZE);
  for (size_t i = 0; i < ehdr.e_phnum; ++i) {
    ElfW(Phdr) *phdr = &phdrs[i];
    if (phdr->p_type == PT_LOAD) {
      if ((phdr->p_vaddr % pagesize) != 0 || (phdr->p_offset % pagesize) != 0) {
        // Both the virtual address and file offset must be page aligned.
        errno = ENOEXEC;
        return;
      }
      if (addr_begin > phdr->p_vaddr)
        addr_begin = phdr->p_vaddr;
      if (addr_end < phdr->p_vaddr + phdr->p_memsz)
        addr_end = phdr->p_vaddr + phdr->p_memsz;
    }
  }
  if (addr_begin > addr_end) {
    // Executable has no program headers.
    errno = ENOEXEC;
    return;
  }

  // Ask the kernel for a contiguous range of memory large enough to fit
  // the entire executable.
  char *base =
      mmap(NULL, addr_end - addr_begin, 0, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED)
    return;
  base -= addr_begin;

  // Scan over the program headers a second time to map all of the
  // regions that need to be loaded.
  for (size_t i = 0; i < ehdr.e_phnum; ++i) {
    ElfW(Phdr) *phdr = &phdrs[i];
    if (phdr->p_type == PT_LOAD) {
      // Map the part of the executable at the beginning of this region.
      char *map_base = base + phdr->p_vaddr;
      if (mmap(map_base, phdr->p_filesz, PROT_WRITE, MAP_PRIVATE | MAP_FIXED,
               fd, phdr->p_offset) == MAP_FAILED)
        return;

      // Up to the next page boundary we may have mapped garbage from
      // the file into the address space. Ensure it is zeroed out.
      char *bss_start = map_base + phdr->p_filesz;
      memset(bss_start, '\0', -(uintptr_t)bss_start % pagesize);

      // Adjust permissions of the mapping to its final value.
      int prot = 0;
      if (phdr->p_flags & PF_R)
        prot |= PROT_READ;
      if (phdr->p_flags & PF_W)
        prot |= PROT_WRITE;
      if (phdr->p_flags & PF_X) {
        prot |= PROT_EXEC;
#if CONFIG_TLS_USE_GSBASE
        // Make the executable writable, so we can patch up TLS access.
        prot |= PROT_WRITE;
#endif
      }
      if (mprotect(map_base, roundup(phdr->p_memsz, pagesize), prot) == -1)
        return;
    }
  }

  // Create an in-memory shared object that is provided to the
  // application. This shared object contains the system call functions
  // that may be invoked.
  struct vdso {
    ElfW(Ehdr) ehdr;
    ElfW(Phdr) phdrs[1];
    ElfW(Dyn) dyns[6];
#define NSYSCALLS (sizeof(*syscalls) / sizeof(void *))
    struct {
      Elf32_Word nbucket;
      Elf32_Word nchain;
      Elf32_Word buckets[1];
      Elf32_Word chains[NSYSCALLS];
    } hash;
#define STRENT(x) \
  "\0"            \
  "cloudabi_sys_" #x
#define STRTAB CLOUDABI_SYSCALL_NAMES(STRENT)
    char strtab[sizeof(STRTAB)];
    ElfW(Sym) symtab[NSYSCALLS];
  } vdso = {
      .ehdr =
          {
              .e_ident =
                  {
                      [EI_MAG0] = ELFMAG0,
                      [EI_MAG1] = ELFMAG1,
                      [EI_MAG2] = ELFMAG2,
                      [EI_MAG3] = ELFMAG3,
                      [EI_OSABI] = ELFOSABI_CLOUDABI,
                  },
              .e_type = ET_DYN,
              .e_machine = ehdr.e_machine,
              .e_version = EV_CURRENT,
              .e_phoff = offsetof(struct vdso, phdrs),
              .e_flags = ehdr.e_flags,
              .e_ehsize = sizeof(vdso.ehdr),
              .e_phentsize = sizeof(vdso.phdrs[0]),
              .e_phnum = sizeof(vdso.phdrs) / sizeof(vdso.phdrs[0]),
          },
      .phdrs =
          {
              {
                  .p_type = PT_DYNAMIC,
                  .p_flags = PF_R,
                  .p_offset = offsetof(struct vdso, dyns),
                  .p_vaddr = offsetof(struct vdso, dyns),
                  .p_filesz = sizeof(vdso.dyns),
                  .p_memsz = sizeof(vdso.dyns),
                  .p_align = _Alignof(ElfW(Dyn)),
              },
          },
      .dyns =
          {
              {.d_tag = DT_HASH, .d_un.d_ptr = offsetof(struct vdso, hash)},
              {.d_tag = DT_STRTAB, .d_un.d_ptr = offsetof(struct vdso, strtab)},
              {.d_tag = DT_SYMTAB, .d_un.d_val = offsetof(struct vdso, symtab)},
              {.d_tag = DT_STRSZ, .d_un.d_val = sizeof(vdso.strtab)},
              {.d_tag = DT_SYMENT, .d_un.d_val = sizeof(vdso.symtab[0])},
              {.d_tag = DT_NULL},
          },
      .hash = {.nbucket = 1, .nchain = NSYSCALLS, .buckets = {0}},
      .strtab = STRTAB,
  };

  // Fill the symbol table and symbol hash table.
  size_t offset = 1;
  for (size_t i = 0; i < NSYSCALLS; ++i) {
    vdso.hash.chains[i] = i + 1 < NSYSCALLS ? i + 1 : STN_UNDEF;
    ElfW(Sym) *sym = &vdso.symtab[i];
    sym->st_name = offset;
    offset += strlen(vdso.strtab + offset) + 1;
    sym->st_info = ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC);
    sym->st_other = STV_DEFAULT;
    sym->st_value = ((const uintptr_t *)&tls_syscalls)[i] - (uintptr_t)&vdso;
  }

#undef STRTAB
#undef NSYSCALLS

  // Create an auxiliary vector containing the parameters that need to
  // be passed to the executable.
  char canary[64];
  random_buf(canary, sizeof(canary));
  char pid[16];
  random_buf(pid, sizeof(pid));
  pid[6] = (pid[6] & 0x0f) | 0x40;
  pid[8] = (pid[8] & 0x3f) | 0x80;
  cloudabi_tid_t tid = tidpool_allocate();
  cloudabi_auxv_t auxv[] = {
      {.a_type = CLOUDABI_AT_ARGDATA, .a_ptr = (void *)argdata},
      {.a_type = CLOUDABI_AT_ARGDATALEN, .a_val = argdatalen},
      {.a_type = CLOUDABI_AT_BASE, .a_ptr = base},
      {.a_type = CLOUDABI_AT_CANARY, .a_ptr = canary},
      {.a_type = CLOUDABI_AT_CANARYLEN, .a_val = sizeof(canary)},
      {.a_type = CLOUDABI_AT_NCPUS, .a_val = sysconf(_SC_NPROCESSORS_ONLN)},
      {.a_type = CLOUDABI_AT_PAGESZ, .a_val = pagesize},
      {.a_type = CLOUDABI_AT_PHDR, .a_ptr = base + ehdr.e_phoff},
      {.a_type = CLOUDABI_AT_PHNUM, .a_val = ehdr.e_phnum},
      {.a_type = CLOUDABI_AT_PID, .a_ptr = pid},
      {.a_type = CLOUDABI_AT_SYSINFO_EHDR, .a_ptr = &vdso},
      {.a_type = CLOUDABI_AT_TID, .a_val = tid},
      {.a_type = CLOUDABI_AT_NULL},
  };

  // Reset signals to their default behaviour.
  signals_init();

#if CONFIG_HAS_CAP_ENTER
  // Make use of the host system's support for Capsicum. By enabling
  // this, there is no need to emulate any of the directory sandboxing
  // features.
  cap_enter();
#endif

  // Set up a new TLS area.
  curtid = tid;
  struct tls tls;
  tls_init(&tls, syscalls);

  // Call into the entry point of the executable, providing it the
  // auxiliary vector.
  void (*start)(const cloudabi_auxv_t *) =
      (void (*)(const cloudabi_auxv_t *))(base + ehdr.e_entry);
  start(auxv);

  // The entry point of the executable may never return.
  abort();
}
