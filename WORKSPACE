load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

git_repository(
    name = "com_github_nuxinl_argdata",
    commit = "686bffe0e2dacf39c4bfe08a5f0d8f30c3237d2f",
    remote = "https://github.com/NuxiNL/argdata.git",
)

git_repository(
    name = "com_github_nuxinl_arpc",
    commit = "58e54234bb0e493b9d3aca5ddc5f1e216083588e",
    remote = "https://github.com/NuxiNL/arpc.git",
)

git_repository(
    name = "com_github_nuxinl_bazel_third_party",
    commit = "1ea7fe04d7444391e388915ba24f1f9b204705ec",
    remote = "https://github.com/NuxiNL/bazel-third-party.git",
)

git_repository(
    name = "com_github_nuxinl_cloudabi",
    commit = "2a847ede6a866844eca6d5c9b695ed6da0ae9c82",
    remote = "https://github.com/NuxiNL/cloudabi.git",
)

git_repository(
    name = "com_github_nuxinl_flower",
    commit = "2c9c423791a55c7ed68c687858bacc344eda5676",
    remote = "https://github.com/NuxiNL/flower.git",
)

git_repository(
    name = "com_github_nuxinl_yaml2argdata",
    commit = "316b1acd4276d68a10782ff0fcd803f2867c38a7",
    remote = "https://github.com/NuxiNL/yaml2argdata.git",
)

load("@com_github_nuxinl_bazel_third_party//:third_party.bzl", "third_party_repositories")

third_party_repositories()

# TODO(ed): Remove the dependencies below once Bazel supports transitive
# dependency loading. These are from ARPC.
# https://github.com/bazelbuild/proposals/blob/master/designs/2018-11-07-design-recursive-workspaces.md

git_repository(
    name = "io_bazel_rules_python",
    commit = "e6399b601e2f72f74e5aa635993d69166784dde1",
    remote = "https://github.com/bazelbuild/rules_python.git",
)

load("@io_bazel_rules_python//python:pip.bzl", "pip_import", "pip_repositories")

pip_repositories()

pip_import(
    name = "aprotoc_deps",
    requirements = "@com_github_nuxinl_arpc//scripts:requirements.txt",
)

load("@aprotoc_deps//:requirements.bzl", "pip_install")

pip_install()
