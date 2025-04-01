/**
 * bookmarkfs/tests/check_util.c
 * ----
 *
 * Copyright (C) 2025  CismonX <admin@cismon.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "check_util.h"

#include <inttypes.h>
#include <stdio.h>

int
prng_seed_from_hex (
    uint64_t   *buf,
    char const *str
) {
    int cnt = sscanf(str,
            "%16" SCNx64 "%16" SCNx64 "%16" SCNx64 "%16" SCNx64,
            &buf[0], &buf[1], &buf[2], &buf[3]);
    if (cnt != 4) {
        log_printf("bad seed '%s'", str);
        return -1;
    }
    return 0;
}
