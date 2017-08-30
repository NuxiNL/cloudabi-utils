// Copyright (c) 2017 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#include <sys/socket.h>
#include <sys/un.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpc++/arpc++.h>
#include <flower/protocol/switchboard.ad.h>
#include <yaml-cpp/exceptions.h>
#include <yaml-cpp/mark.h>
#include <argdata.hpp>

#include "yaml_file_descriptor_factory.h"

using arpc::ArgdataParser;
using arpc::Channel;
using arpc::ClientContext;
using arpc::CreateChannel;
using arpc::FileDescriptor;
using arpc::Status;
using cloudabi_run::YAMLFileDescriptorFactory;
using flower::protocol::switchboard::ConstrainRequest;
using flower::protocol::switchboard::ConstrainResponse;
using flower::protocol::switchboard::Switchboard;

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

const argdata_t *YAMLFileDescriptorFactory::GetMap(
    const YAML::Mark &mark, std::string_view tag,
    std::vector<const argdata_t *> keys,
    std::vector<const argdata_t *> values) {
  if (tag == "tag:nuxi.nl,2015:cloudabi/file") {
    // Open a file by pathname.
    // TODO(ed): Make mode and rights adjustable.
    std::optional<std::string_view> path;
    for (size_t i = 0; i < keys.size(); ++i) {
      std::optional<std::string_view> keystr = keys[i]->get_str();
      if (!keystr)
        throw YAML::ParserException(mark, "Expected string keys");
      if (*keystr == "path") {
        path = values[i]->get_str();
      } else {
        std::ostringstream ss;
        ss << "Unsupported attribute: " << *keystr;
        throw YAML::ParserException(mark, ss.str());
      }
    }
    if (!path)
      throw YAML::ParserException(mark, "Missing path attribute");

    int fd = open(std::string(*path).c_str(), O_RDONLY);
    if (fd < 0) {
      std::ostringstream ss;
      ss << "Failed to open \"" << *path << "\": " << std::strerror(errno);
      throw YAML::ParserException(mark, ss.str());
    }
    return argdatas_
        .emplace_back(argdata_t::create_fd(
            fds_.emplace_back(std::make_unique<FileDescriptor>(fd))->get()))
        .get();
  } else if (tag == "tag:nuxi.nl,2015:cloudabi/flower_switchboard_handle") {
    // Create a Flower switchboard handle for network connectivity.
    std::optional<std::string_view> switchboard_path;
    ArgdataParser parser;
    ConstrainRequest request;
    for (size_t i = 0; i < keys.size(); ++i) {
      std::optional<std::string_view> keystr = keys[i]->get_str();
      if (!keystr)
        throw YAML::ParserException(mark, "Expected string keys");
      if (*keystr == "switchboard_path") {
        switchboard_path = values[i]->get_str();
      } else if (*keystr == "constraints") {
        request.Parse(*values[i], &parser);
      } else {
        std::ostringstream ss;
        ss << "Unsupported attribute: " << *keystr;
        throw YAML::ParserException(mark, ss.str());
      }
    }
    if (!switchboard_path)
      throw YAML::ParserException(mark, "Missing switchboard_path attribute");

    // Connect to the switchboard.
    std::unique_ptr<FileDescriptor> root_handle;
    {
      int s = socket(AF_UNIX, SOCK_STREAM, 0);
      if (s < 0)
        throw YAML::ParserException(
            mark,
            std::string("Failed to create socket: ") + std::strerror(errno));
      root_handle = std::make_unique<FileDescriptor>(s);
      union {
        struct sockaddr_un sun;
        struct sockaddr sa;
      } address = {};
      address.sun.sun_family = AF_UNIX;
      if (switchboard_path->size() >= std::size(address.sun.sun_path))
        throw YAML::ParserException(mark, "Switchboard path too long");
      std::copy(switchboard_path->begin(), switchboard_path->end(),
                address.sun.sun_path);
      if (connect(root_handle->get(), &address.sa, sizeof(address)) != 0)
        throw YAML::ParserException(
            mark, std::string("Failed to connect to the switchboard: ") +
                      std::strerror(errno));
    }

    // Create a constrained channel.
    std::shared_ptr<Channel> channel = CreateChannel(std::move(root_handle));
    std::unique_ptr<Switchboard::Stub> stub = Switchboard::NewStub(channel);
    ClientContext context;
    ConstrainResponse response;
    if (Status status = stub->Constrain(&context, request, &response);
        !status.ok())
      throw YAML::ParserException(
          mark, std::string("Failed to constrain switchboard channel: ") +
                    status.error_message());
    if (!response.switchboard())
      throw YAML::ParserException(
          mark, "Switchboard did not return a file descriptor");

    return argdatas_
        .emplace_back(argdata_t::create_fd(
            fds_.emplace_back(response.switchboard())->get()))
        .get();
  } else {
    return fallback_->GetMap(mark, tag, std::move(keys), std::move(values));
  }
}
