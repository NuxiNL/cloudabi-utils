// Copyright (c) 2017-2018 Nuxi, https://nuxi.nl/
//
// SPDX-License-Identifier: BSD-2-Clause

#ifndef CLOUDABI_RUN_YAML_FILE_DESCRIPTOR_FACTORY_H
#define CLOUDABI_RUN_YAML_FILE_DESCRIPTOR_FACTORY_H

#include <string_view>
#include <vector>

#include <arpc++/arpc++.h>
#include <yaml-cpp/mark.h>
#include <yaml2argdata/yaml_factory.h>
#include <argdata.hpp>

namespace cloudabi_run {

class YAMLFileDescriptorFactory
    : public yaml2argdata::YAMLFactory<const argdata_t *> {
 public:
  YAMLFileDescriptorFactory(YAMLFactory<const argdata_t *> *fallback, int execfd)
      : fallback_(fallback), execfd_(execfd) {
  }

  const argdata_t *GetNull(const YAML::Mark &mark) override;
  const argdata_t *GetScalar(const YAML::Mark &mark, std::string_view tag,
                             std::string_view value) override;
  const argdata_t *GetSequence(
      const YAML::Mark &mark, std::string_view tag,
      std::vector<const argdata_t *> elements) override;
  const argdata_t *GetMap(const YAML::Mark &mark, std::string_view tag,
                          std::vector<const argdata_t *> keys,
                          std::vector<const argdata_t *> values) override;

 private:
  YAMLFactory<const argdata_t *> *const fallback_;
  const int execfd_;

  std::vector<std::unique_ptr<argdata_t>> argdatas_;
  std::vector<std::shared_ptr<arpc::FileDescriptor>> fds_;
};

}  // namespace cloudabi_run

#endif
