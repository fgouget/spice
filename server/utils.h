/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009-2015 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#ifndef UTILS_H_
#define UTILS_H_

#include <stdint.h>
#include <glib.h>

#define SPICE_GNUC_VISIBLE __attribute__ ((visibility ("default")))

static inline void set_bit(int index, uint32_t *addr)
{
    uint32_t mask = 1 << index;
    __sync_or_and_fetch(addr, mask);
}

static inline void clear_bit(int index, uint32_t *addr)
{
    uint32_t mask = ~(1 << index);
    __sync_and_and_fetch(addr, mask);
}

static inline int test_bit(int index, uint32_t val)
{
    return val & (1u << index);
}
/* a generic safe for loop macro  */
#define SAFE_FOREACH(link, next, cond, ring, data, get_data)            \
    for ((((link) = ((cond) ? ring_get_head(ring) : NULL)),             \
          ((next) = ((link) ? ring_next((ring), (link)) : NULL)),       \
          ((data) = ((link)? (get_data) : NULL)));                      \
         (link);                                                        \
         (((link) = (next)),                                            \
          ((next) = ((link) ? ring_next((ring), (link)) : NULL)),       \
          ((data) = ((link)? (get_data) : NULL))))

typedef int64_t red_time_t;

#define NSEC_PER_SEC      1000000000LL
#define NSEC_PER_MILLISEC 1000000LL
#define NSEC_PER_MICROSEC 1000

/* g_get_monotonic_time() does not have enough precision */
static inline red_time_t spice_get_monotonic_time_ns(void)
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);
    return NSEC_PER_SEC * time.tv_sec + time.tv_nsec;
}

static inline red_time_t spice_get_real_time_ns(void)
{
    struct timespec time;

    clock_gettime(CLOCK_REALTIME, &time);
    return NSEC_PER_SEC * time.tv_sec + time.tv_nsec;
}

#define MSEC_PER_SEC 1000

int rgb32_data_has_alpha(int width, int height, size_t stride,
                         const uint8_t *data, int *all_set_out);

const char *red_channel_type_to_str(int type);
int red_channel_name_to_type(const char *name);

void red_dump_openssl_errors(void);

#endif /* UTILS_H_ */
