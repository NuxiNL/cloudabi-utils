// Copyright (c) 2015 Nuxi, https://nuxi.nl/
//
// This file is distrbuted under a 2-clause BSD license.
// See the LICENSE file for details.

#include <argdata.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "argdata_impl.h"

int argdata_iterate_map(const argdata_t *ad,
                        bool (*iterator)(const argdata_t *, const argdata_t *,
                                         void *),
                        void *thunk) {
  switch (ad->type) {
    case AD_BUFFER: {
      const uint8_t *buf = ad->buffer;
      size_t len = ad->length;
      int error = parse_type(ADT_MAP, &buf, &len);
      if (error != 0)
        return error;

      while (len > 0) {
        // Parse the key of the map entry.
        argdata_t key;
        error = parse_subfield(&key, &buf, &len);
        if (error != 0)
          return error;

        // Parse the value of the map entry.
        argdata_t value;
        error = parse_subfield(&value, &buf, &len);
        if (error != 0)
          return error;

        // Invoke iterator function with map entry.
        if (!iterator(&key, &value, thunk))
          break;
      }
      return 0;
    }
    case AD_MAP:
      for (size_t i = 0; i < ad->map.count; ++i)
        if (!iterator(ad->map.keys[i], ad->map.values[i], thunk))
          break;
      return 0;
    default:
      return EINVAL;
  }
}
