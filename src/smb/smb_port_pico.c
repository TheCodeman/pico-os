/**
 * @file smb_port_pico.c
 * @brief What smb.c needs from the board: random numbers and the time.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#include <time.h>

#include "pico/rand.h"
#include "smb.h"

void smb_random(void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) {
        uint32_t r = get_rand_32();
        for (int i = 0; i < 4 && len; ++i, --len)
            *p++ = r >> (8 * i);
    }
}

uint64_t smb_unix_time(void) {
    time_t t = time(NULL);
    return t > 1700000000 ? (uint64_t)t : 0;  // 0: the clock isn't set
}
