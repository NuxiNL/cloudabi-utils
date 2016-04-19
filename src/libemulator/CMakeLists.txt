find_package(Threads REQUIRED)

add_library(emulator STATIC
            emulate.c futex.c posix.c random.c signals.c str.c tidpool.c tls.c)
target_link_libraries(emulator ${CMAKE_THREAD_LIBS_INIT})
