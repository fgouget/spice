/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009,2010 Red Hat, Inc.

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

#ifndef MEMSLOT_H_
#define MEMSLOT_H_

#include <spice/qxl_dev.h>

#include "red-common.h"

typedef struct MemSlot {
    int generation;
    unsigned long virt_start_addr;
    unsigned long virt_end_addr;
    long address_delta;
} MemSlot;

typedef struct RedMemSlotInfo {
    MemSlot **mem_slots;
    uint32_t num_memslots_groups;
    uint32_t num_memslots;
    uint8_t mem_slot_bits;
    uint8_t generation_bits;
    uint8_t memslot_id_shift;
    uint8_t memslot_gen_shift;
    uint8_t internal_groupslot_id;
    unsigned long memslot_gen_mask;
    unsigned long memslot_clean_virt_mask;
} RedMemSlotInfo;

static inline int memslot_get_id(RedMemSlotInfo *info, uint64_t addr)
{
    return addr >> info->memslot_id_shift;
}

static inline int memslot_get_generation(RedMemSlotInfo *info, uint64_t addr)
{
    return (addr >> info->memslot_gen_shift) & info->memslot_gen_mask;
}

int memslot_validate_virt(RedMemSlotInfo *info, unsigned long virt, int slot_id,
                          uint32_t add_size, uint32_t group_id);
unsigned long memslot_max_size_virt(RedMemSlotInfo *info,
                                    unsigned long virt, int slot_id,
                                    uint32_t group_id);
unsigned long memslot_get_virt(RedMemSlotInfo *info, QXLPHYSICAL addr, uint32_t add_size,
                       int group_id, int *error);

void memslot_info_init(RedMemSlotInfo *info,
                       uint32_t num_groups, uint32_t num_slots,
                       uint8_t generation_bits,
                       uint8_t id_bits,
                       uint8_t internal_groupslot_id);
void memslot_info_destroy(RedMemSlotInfo *info);
void memslot_info_add_slot(RedMemSlotInfo *info, uint32_t slot_group_id, uint32_t slot_id,
                           uint64_t addr_delta, unsigned long virt_start, unsigned long virt_end,
                           uint32_t generation);
void memslot_info_del_slot(RedMemSlotInfo *info, uint32_t slot_group_id, uint32_t slot_id);
void memslot_info_reset(RedMemSlotInfo *info);

#endif /* MEMSLOT_H_ */
