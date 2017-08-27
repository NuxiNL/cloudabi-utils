// Copyright (c) 2017 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#ifndef CLOUDABI_RUN_YAML_FILE_DESCRIPTOR_FACTORY_H
#define CLOUDABI_RUN_YAML_FILE_DESCRIPTOR_FACTORY_H

#include <string_view>
#include <vector>

#include <argdata.hpp>
#include <yaml-cpp/mark.h>
#include <yaml2argdata/yaml_factory.h>

namespace cloudabi_run {

class YAMLFileDescriptorFactory
    : public yaml2argdata::YAMLFactory<const argdata_t *> {
public:
  YAMLFileDescriptorFactory(YAMLFactory<const argdata_t *> *fallback)
      : fallback_(fallback) {}

  const argdata_t *GetNull(const YAML::Mark &mark) override;
  const argdata_t *GetScalar(const YAML::Mark &mark, std::string_view tag,
                             std::string_view value) override;
  const argdata_t *
  GetSequence(const YAML::Mark &mark, std::string_view tag,
              std::vector<const argdata_t *> elements) override;
  const argdata_t *GetMap(const YAML::Mark &mark, std::string_view tag,
                          std::vector<const argdata_t *> keys,
                          std::vector<const argdata_t *> values) override;

private:
  YAMLFactory<const argdata_t *> *const fallback_;

  std::vector<std::unique_ptr<argdata_t>> argdatas_;
};

} // namespace cloudabi_run

#endif
