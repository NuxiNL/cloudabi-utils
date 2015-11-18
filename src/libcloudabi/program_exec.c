// Copyright (c) 2015 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#include <argdata.h>
#include <errno.h>
#include <program.h>
#include <string.h>
#include <unistd.h>

int program_exec(int fd, const argdata_t *ad) {
  // Place file descriptor and arguments data in a sequence.
  argdata_t *adfd = argdata_create_fd(fd);
  if (adfd == NULL)
    return errno;
  const argdata_t *seq[] = {adfd, ad};
  argdata_t *adseq = argdata_create_seq(seq, sizeof(seq) / sizeof(seq[0]));
  if (adseq == NULL) {
    argdata_free(adfd);
    return errno;
  }

  // Encode data. Add a trailing null byte, as execve() uses null
  // terminated strings.
  size_t datalen;
  argdata_get_buffer_length(adseq, &datalen, NULL);
  char data[datalen + 1];
  argdata_get_buffer(adseq, data, NULL);
  data[datalen] = '\0';
  argdata_free(adfd);
  argdata_free(adseq);

  // Data may contain null bytes. Split data up in multiple arguments,
  // so that all arguments concatenated (including the null bytes)
  // correspond to the original data.
  size_t argc = 0;
  for (size_t i = 0; i <= datalen; ++i)
    if (data[i] == '\0')
      ++argc;
  char *argv[argc + 1];
  char *p = data;
  for (size_t i = 0; i < argc; ++i) {
    argv[i] = p;
    p += strlen(p) + 1;
  }
  argv[argc] = NULL;

  // The environment can just be empty, as CloudABI processes don't have
  // environment variables.
  char *envp = NULL;

  // Don't execute the program directly, but call through
  // cloudabi-reexec first. This program calls the native CloudABI
  // program_exec() function with the arguments provided. The native
  // function makes sure that no other file descriptors leak into the
  // sandboxed program. This also ensures that we're already in
  // capabilities mode before executing the program.
  execve(PATH_CLOUDABI_REEXEC, argv, &envp);
  return errno;
}
