include(GNUInstallDirs)

add_library(cloudabi SHARED
            argdata_create_binary.c argdata_create_buffer.c
            argdata_create_fd.c argdata_create_float.c
            argdata_create_int_s.c argdata_create_int_u.c
            argdata_create_map.c argdata_create_seq.c
            argdata_create_str.c argdata_create_str_c.c
            argdata_create_timestamp.c argdata_false.c argdata_free.c
            argdata_get_binary.c argdata_get_bool.c argdata_get_buffer.c
            argdata_get_buffer_length.c argdata_get_fd.c
            argdata_get_float.c argdata_get_int_s.c argdata_get_int_u.c
            argdata_get_str.c argdata_get_str_c.c
            argdata_get_timestamp.c argdata_map_iterate.c
            argdata_map_next.c argdata_null.c argdata_seq_iterate.c
            argdata_seq_next.c argdata_true.c program_exec.c)
add_definitions(-DPATH_CLOUDABI_REEXEC="${CMAKE_INSTALL_FULL_LIBEXECDIR}/cloudabi-reexec")

set_property(TARGET cloudabi PROPERTY VERSION "1")
install(TARGETS cloudabi LIBRARY
        DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES argdata.h program.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cloudabi)
