add_library(emulator STATIC
            emulate.c futex.c posix.c random.c signals.c str.c tidpool.c tls.c)
