// Copyright (c) 2016 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#ifndef RANDOM_H
#define RANDOM_H

#include <stdint.h>

void random_buf(void *, size_t);
uintmax_t random_uniform(uintmax_t);

#endif
