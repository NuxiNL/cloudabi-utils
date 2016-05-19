#!/bin/sh
x86_64-unknown-cloudabi-cc -O2 -Wall -Werror -I../libemulator \
    -o cloudabi-emulate \
    ../libemulator/emulate.c \
    ../libemulator/futex.c \
    ../libemulator/posix.c \
    ../libemulator/random.c \
    ../libemulator/signals.c \
    ../libemulator/str.c \
    ../libemulator/tidpool.c \
    ../libemulator/tls.c \
    cloudabi-emulate.c
