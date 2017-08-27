// Copyright (c) 2017 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <argdata.hpp>
#include <yaml-cpp/exceptions.h>
#include <yaml-cpp/mark.h>

#include "yaml_file_descriptor_factory.h"

using cloudabi_run::YAMLFileDescriptorFactory;

const argdata_t *YAMLFileDescriptorFactory::GetNull(const YAML::Mark &mark) {
  return fallback_->GetNull(mark);
}

const argdata_t *YAMLFileDescriptorFactory::GetScalar(const YAML::Mark &mark,
                                                      std::string_view tag,
                                                      std::string_view value) {
  if (tag == "tag:nuxi.nl,2015:cloudabi/fd") {
    // Insert an already existing file descriptor.
    int fd;
    if (value == "stdout") {
      fd = STDOUT_FILENO;
    } else if (value == "stderr") {
      fd = STDERR_FILENO;
    } else {
      // TODO(ed): Implement!
      throw YAML::ParserException(mark, "XXX!");
    }
    return argdatas_.emplace_back(argdata_t::create_fd(fd)).get();
  } else {
    return fallback_->GetScalar(mark, tag, value);
  }
}

const argdata_t *YAMLFileDescriptorFactory::GetSequence(
    const YAML::Mark &mark, std::string_view tag,
    std::vector<const argdata_t *> elements) {
  return fallback_->GetSequence(mark, tag, std::move(elements));
}

const argdata_t *
YAMLFileDescriptorFactory::GetMap(const YAML::Mark &mark, std::string_view tag,
                                  std::vector<const argdata_t *> keys,
                                  std::vector<const argdata_t *> values) {
  if (tag == "tag:nuxi.nl,2015:cloudabi/file") {
    // Open a file by pathname.
    // TODO(ed): Make mode and rights adjustable.
    std::optional<std::string_view> path;
    for (size_t i = 0; i < keys.size(); ++i) {
      if (keys[i]->as_str() == "path")
        path = values[i]->get_str();
    }
    if (!path)
      throw YAML::ParserException(mark, "Missing path attribute");

    int fd = open(std::string(*path).c_str(), O_RDONLY);
    if (fd < 0) {
      std::ostringstream ss;
      ss << "Failed to open \"" << *path << "\": " << std::strerror(errno);
      throw YAML::ParserException(mark, ss.str());
    }
    return argdatas_.emplace_back(argdata_t::create_fd(fd)).get();
  } else {
    return fallback_->GetMap(mark, tag, std::move(keys), std::move(values));
  }
}
