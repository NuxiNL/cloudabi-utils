#!/bin/sh
WARNFLAGS="-Werror -Werror-pointer-arith -Wall -Wsystem-headers \
-Wold-style-definition -Wreturn-type -Wwrite-strings -Wswitch \
-Wchar-subscripts -Wnested-externs -Wshadow -Wmissing-prototypes \
-Wstrict-prototypes -Wmissing-variable-declarations -Wthread-safety \
-Wsign-compare -Wundef -Wno-comment -Wno-macro-redefined"
x86_64-unknown-cloudabi-cc -O2 ${WARNFLAGS} -I../libemulator \
    -o cloudabi-emulate cloudabi-emulate.c ../libemulator/*.c
