include(FindPkgConfig)
include(GNUInstallDirs)

pkg_search_module(YAML REQUIRED yaml yaml-0.1)

include_directories(${YAML_INCLUDE_DIRS})
link_directories(${YAML_LIBRARY_DIRS})

add_executable(cloudabi-run cloudabi-run.c)
target_link_libraries(cloudabi-run cloudabi emulator ${YAML_LIBRARIES})

install(TARGETS cloudabi-run
        DESTINATION ${CMAKE_INSTALL_BINDIR})
INSTALL(FILES cloudabi-run.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
