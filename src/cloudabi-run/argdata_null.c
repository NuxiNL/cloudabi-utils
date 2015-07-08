// Copyright (c) 2015 Nuxi, https://nuxi.nl/
//
// This file is distrbuted under a 2-clause BSD license.
// See the LICENSE file for details.

#include <stddef.h>

#include "argdata.h"
#include "argdata_impl.h"

const argdata_t argdata_null = {
    .type = AD_BUFFER, .buffer = NULL, .length = 0,
};
