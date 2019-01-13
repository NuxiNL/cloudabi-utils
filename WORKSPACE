workspace(name = "org_cloudabi_cloudabi_utils")

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

git_repository(
    name = "org_cloudabi_argdata",
    commit = "191ab391fbe0be3edbee59bedd73165de9b3abf5",
    remote = "https://github.com/NuxiNL/argdata.git",
)

git_repository(
    name = "org_cloudabi_arpc",
    commit = "81305e311c0559fe7a64a98ce8ac1aa7051c7a4d",
    remote = "https://github.com/NuxiNL/arpc.git",
)

git_repository(
    name = "org_cloudabi_bazel_third_party",
    commit = "8d51abda2299d5fe26ca7c55f182b6562f440979",
    remote = "https://github.com/NuxiNL/bazel-third-party.git",
)

git_repository(
    name = "org_cloudabi_cloudabi",
    commit = "af51ede669dbca0875d20893dae7f760b052b238",
    remote = "https://github.com/NuxiNL/cloudabi.git",
)

git_repository(
    name = "org_cloudabi_flower",
    commit = "fac0e618f3ca566ac0ba07544496f388268c6316",
    remote = "https://github.com/NuxiNL/flower.git",
)

git_repository(
    name = "org_cloudabi_yaml2argdata",
    commit = "678be75c7a9bb23c80a8de4bbbccf26bba9570aa",
    remote = "https://github.com/NuxiNL/yaml2argdata.git",
)

load("@org_cloudabi_bazel_third_party//:third_party.bzl", "third_party_repositories")

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
    requirements = "@org_cloudabi_arpc//scripts:requirements.txt",
)

load("@aprotoc_deps//:requirements.bzl", "pip_install")

pip_install()
