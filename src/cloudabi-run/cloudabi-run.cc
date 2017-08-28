// Copyright (c) 2015-2017 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

// cloudabi-run - execute CloudABI programs safely
//
// The cloudabi-run utility can execute CloudABI programs with an exact
// set of file descriptors. It reads a YAML configuration from stdin.
// This data is converted to argument data (argdata_t) that can be
// accessed from program_main().
//
// !fd, !file and !socket nodes in the YAML file are converted to file
// descriptor entries in the argument data, meaning they will be
// available within the CloudABI process.

#include <argdata.h>
#include <fcntl.h>
#ifndef O_EXEC
#define O_EXEC O_RDONLY
#endif
#include <program.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

#include <yaml2argdata/yaml_argdata_factory.h>
#include <yaml2argdata/yaml_builder.h>
#include <yaml2argdata/yaml_canonicalizing_factory.h>
#include <yaml2argdata/yaml_error_factory.h>

#include "yaml_file_descriptor_factory.h"

extern "C" {
#include "../libemulator/emulate.h"
#include "../libemulator/posix.h"
}

using cloudabi_run::YAMLFileDescriptorFactory;
using yaml2argdata::YAMLArgdataFactory;
using yaml2argdata::YAMLBuilder;
using yaml2argdata::YAMLCanonicalizingFactory;
using yaml2argdata::YAMLErrorFactory;

namespace {

[[noreturn]] void usage(void) {
  std::cerr << "usage: cloudabi-run [-e] executable" << std::endl;
  std::exit(127);
}

}  // namespace

int main(int argc, char *argv[]) {
  // Parse command line options.
  bool do_emulate = false;
  int c;
  while ((c = getopt(argc, argv, "e")) != -1) {
    switch (c) {
      case 'e':
        // Run program using emulation.
        do_emulate = true;
        break;
      default:
        usage();
    }
  }
  argv += optind;
  argc -= optind;
  if (argc != 1)
    usage();

  // Parse YAML configuration.
  YAMLErrorFactory<const argdata_t *> error_factory;
  YAMLFileDescriptorFactory file_descriptor_factory(&error_factory);
  YAMLArgdataFactory argdata_factory(&file_descriptor_factory);
  YAMLCanonicalizingFactory<const argdata_t *> canonicalizing_factory(
      &argdata_factory);
  YAMLBuilder<const argdata_t *> builder(&canonicalizing_factory);
  const argdata_t *ad;
  try {
    ad = builder.Build(&std::cin);
  } catch (std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 127;
  }

  if (do_emulate) {
    // Serialize argument data that needs to be passed to the executable.
    std::vector<unsigned char> buf;
    std::vector<int> fds;
    {
      size_t buflen, fdslen;
      argdata_serialized_length(ad, &buflen, &fdslen);
      buf.resize(buflen);
      fds.resize(fdslen);
      fds.resize(argdata_serialize(ad, buf.data(), fds.data()));
    }

    // Register file descriptors.
    struct fd_table ft;
    fd_table_init(&ft);
    for (size_t i = 0; i < fds.size(); ++i) {
      if (!fd_table_insert_existing(&ft, i, fds[i])) {
        std::cerr << "Failed to register file descriptor in argument data: "
                  << std::strerror(errno) << std::endl;
        return 127;
      }
    }

    // Call into the emulator to run the program inside of this process.
    // Throw a warning message before beginning execution, as emulation
    // is not considered secure.
    int fd = open(argv[0], O_RDONLY);
    if (fd == -1) {
      std::cerr << "Failed to open executable: " << std::strerror(errno)
                << std::endl;
      return 127;
    }
    std::cerr
        << "WARNING: Attempting to start executable using emulation.\n"
           "Keep in mind that this emulation provides no actual sandboxing.\n"
           "Though this is likely no problem for development and testing\n"
           "purposes, using this emulator in production is strongly\n"
           "discouraged."
        << std::endl;

    emulate(fd, buf.data(), buf.size(), &posix_syscalls);
  } else {
    // Execute the application directly through the operating system.
    int fd = open(argv[0], O_EXEC);
    if (fd == -1) {
      std::cerr << "Failed to open executable: " << std::strerror(errno)
                << std::endl;
      return 127;
    }
    errno = program_exec(fd, ad);
  }
  std::cerr << "Failed to start executable: " << std::strerror(errno)
            << std::endl;
  return 127;
}
