#!/bin/sh
x86_64-unknown-cloudabi-cc -O2 -Wall -Werror -I../libemulator \
    -o cloudabi-emulate cloudabi-emulate.c ../libemulator/*.c
