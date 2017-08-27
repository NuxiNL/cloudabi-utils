include(FindPkgConfig)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(cloudabi-run cloudabi-run.cc yaml_file_descriptor_factory.cc)
target_link_libraries(cloudabi-run cloudabi emulator yaml-cpp)

install(TARGETS cloudabi-run
        DESTINATION ${CMAKE_INSTALL_BINDIR})
INSTALL(FILES cloudabi-run.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
