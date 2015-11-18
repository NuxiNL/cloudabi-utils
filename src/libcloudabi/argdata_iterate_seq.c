// Copyright (c) 2015 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#include <argdata.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "argdata_impl.h"

int argdata_iterate_seq(const argdata_t *ad,
                        bool (*iterator)(const argdata_t *, void *),
                        void *thunk) {
  switch (ad->type) {
    case AD_BUFFER: {
      const uint8_t *buf = ad->buffer;
      size_t len = ad->length;
      int error = parse_type(ADT_SEQ, &buf, &len);
      if (error != 0)
        return error;

      while (len > 0) {
        // Parse the value of the sequence entry.
        argdata_t value;
        error = parse_subfield(&value, &buf, &len);
        if (error != 0)
          return error;

        // Invoke iterator function with sequence entry.
        if (!iterator(&value, thunk))
          break;
      }
      return 0;
    }
    case AD_SEQ:
      for (size_t i = 0; i < ad->seq.count; ++i)
        if (!iterator(ad->seq.entries[i], thunk))
          break;
      return 0;
    default:
      return EINVAL;
  }
}
