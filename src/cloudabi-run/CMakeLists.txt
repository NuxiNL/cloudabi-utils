add_executable(cloudabi-run cloudabi-run.c)

find_package(PkgConfig REQUIRED)
pkg_search_module(YAML REQUIRED yaml yaml-0.1)

target_link_libraries(cloudabi-run ${YAML_LIBRARIES})
target_include_directories(cloudabi-run PUBLIC ${YAML_INCLUDE_DIRS})
target_compile_options(cloudabi-run PUBLIC ${YAML_CFLAGS_OTHER})
