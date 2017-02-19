include(GNUInstallDirs)

add_library(cloudabi SHARED
            program_exec.c)
add_definitions(-DPATH_CLOUDABI_REEXEC="${CMAKE_INSTALL_FULL_LIBEXECDIR}/cloudabi-reexec")

find_library(ARGDATA_LIBRARY argdata)
target_link_libraries(cloudabi ${ARGDATA_LIBRARY})

set_property(TARGET cloudabi PROPERTY VERSION "1")
install(TARGETS cloudabi LIBRARY
        DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES program.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cloudabi)
