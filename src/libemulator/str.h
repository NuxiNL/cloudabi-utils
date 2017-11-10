// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// SPDX-License-Identifier: BSD-2-Clause

#ifndef STR_H
#define STR_H

#include "config.h"

#if CONFIG_HAS_STRLCPY
#include <string.h>
#else
size_t strlcpy(char *, const char *, size_t);
#endif

char *str_nullterminate(const char *, size_t);

#endif
