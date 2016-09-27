/*
 *  Copyright (C) 2014 Red Hat, Inc.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SPICE_QXL_H_
#define SPICE_QXL_H_

#if !defined(SPICE_H_INSIDE) && !defined(SPICE_SERVER_INTERNAL)
#error "Only spice.h can be included directly."
#endif

#include "spice-core.h"

#ifndef SPICE_CAPABILITIES_SIZE
#define SPICE_CAPABILITIES_SIZE (sizeof(((QXLRom*)0)->client_capabilities))
#endif

/* qxl interface */

#define SPICE_INTERFACE_QXL "qxl"
#define SPICE_INTERFACE_QXL_MAJOR 3
#define SPICE_INTERFACE_QXL_MINOR 3

typedef struct QXLInterface QXLInterface;
typedef struct QXLInstance QXLInstance;
typedef struct QXLState QXLState;
typedef struct QXLWorker QXLWorker;
typedef struct QXLDevMemSlot QXLDevMemSlot;
typedef struct QXLDevSurfaceCreate QXLDevSurfaceCreate;

struct QXLWorker {
    uint32_t minor_version;
    uint32_t major_version;
    /* These calls are deprecated. Please use the spice_qxl_* calls instead */
    void (*wakeup)(QXLWorker *worker) SPICE_GNUC_DEPRECATED;
    void (*oom)(QXLWorker *worker) SPICE_GNUC_DEPRECATED;
    void (*start)(QXLWorker *worker) SPICE_GNUC_DEPRECATED;
    void (*stop)(QXLWorker *worker) SPICE_GNUC_DEPRECATED;
    void (*update_area)(QXLWorker *qxl_worker, uint32_t surface_id,
                       struct QXLRect *area, struct QXLRect *dirty_rects,
                       uint32_t num_dirty_rects, uint32_t clear_dirty_region) SPICE_GNUC_DEPRECATED;
    void (*add_memslot)(QXLWorker *worker, QXLDevMemSlot *slot) SPICE_GNUC_DEPRECATED;
    void (*del_memslot)(QXLWorker *worker, uint32_t slot_group_id, uint32_t slot_id) SPICE_GNUC_DEPRECATED;
    void (*reset_memslots)(QXLWorker *worker) SPICE_GNUC_DEPRECATED;
    void (*destroy_surfaces)(QXLWorker *worker) SPICE_GNUC_DEPRECATED;
    void (*destroy_primary_surface)(QXLWorker *worker, uint32_t surface_id) SPICE_GNUC_DEPRECATED;
    void (*create_primary_surface)(QXLWorker *worker, uint32_t surface_id,
                                   QXLDevSurfaceCreate *surface) SPICE_GNUC_DEPRECATED;
    void (*reset_image_cache)(QXLWorker *worker) SPICE_GNUC_DEPRECATED;
    void (*reset_cursor)(QXLWorker *worker) SPICE_GNUC_DEPRECATED;
    void (*destroy_surface_wait)(QXLWorker *worker, uint32_t surface_id) SPICE_GNUC_DEPRECATED;
    void (*loadvm_commands)(QXLWorker *worker, struct QXLCommandExt *ext, uint32_t count) SPICE_GNUC_DEPRECATED;
};

void spice_qxl_wakeup(QXLInstance *instance);
void spice_qxl_oom(QXLInstance *instance);
/* deprecated since 0.11.2, spice_server_vm_start replaces it */
void spice_qxl_start(QXLInstance *instance) SPICE_GNUC_DEPRECATED;
/* deprecated since 0.11.2 spice_server_vm_stop replaces it */
void spice_qxl_stop(QXLInstance *instance) SPICE_GNUC_DEPRECATED;
void spice_qxl_update_area(QXLInstance *instance, uint32_t surface_id,
                   struct QXLRect *area, struct QXLRect *dirty_rects,
                   uint32_t num_dirty_rects, uint32_t clear_dirty_region);
void spice_qxl_add_memslot(QXLInstance *instance, QXLDevMemSlot *slot);
void spice_qxl_del_memslot(QXLInstance *instance, uint32_t slot_group_id, uint32_t slot_id);
void spice_qxl_reset_memslots(QXLInstance *instance);
void spice_qxl_destroy_surfaces(QXLInstance *instance);
void spice_qxl_destroy_primary_surface(QXLInstance *instance, uint32_t surface_id);
void spice_qxl_create_primary_surface(QXLInstance *instance, uint32_t surface_id,
                               QXLDevSurfaceCreate *surface);
void spice_qxl_reset_image_cache(QXLInstance *instance);
void spice_qxl_reset_cursor(QXLInstance *instance);
void spice_qxl_destroy_surface_wait(QXLInstance *instance, uint32_t surface_id);
void spice_qxl_loadvm_commands(QXLInstance *instance, struct QXLCommandExt *ext, uint32_t count);
/* async versions of commands. when complete spice calls async_complete */
void spice_qxl_update_area_async(QXLInstance *instance, uint32_t surface_id, QXLRect *qxl_area,
                                 uint32_t clear_dirty_region, uint64_t cookie);
void spice_qxl_add_memslot_async(QXLInstance *instance, QXLDevMemSlot *slot, uint64_t cookie);
void spice_qxl_destroy_surfaces_async(QXLInstance *instance, uint64_t cookie);
void spice_qxl_destroy_primary_surface_async(QXLInstance *instance, uint32_t surface_id, uint64_t cookie);
void spice_qxl_create_primary_surface_async(QXLInstance *instance, uint32_t surface_id,
                                QXLDevSurfaceCreate *surface, uint64_t cookie);
void spice_qxl_destroy_surface_async(QXLInstance *instance, uint32_t surface_id, uint64_t cookie);
/* suspend and resolution change on windows drivers */
void spice_qxl_flush_surfaces_async(QXLInstance *instance, uint64_t cookie);
/* since spice 0.12.0 */
void spice_qxl_monitors_config_async(QXLInstance *instance, QXLPHYSICAL monitors_config,
                                     int group_id, uint64_t cookie);
/* since spice 0.12.3 */
void spice_qxl_driver_unload(QXLInstance *instance);
/* since spice 0.12.6, deprecated since 0.14.2, spice_qxl_set_device_info replaces it */
void spice_qxl_set_max_monitors(QXLInstance *instance,
                                unsigned int max_monitors) SPICE_GNUC_DEPRECATED;
/* since spice 0.13.1 */
void spice_qxl_gl_scanout(QXLInstance *instance,
                          int fd,
                          uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t format,
                          int y_0_top);
void spice_qxl_gl_draw_async(QXLInstance *instance,
                             uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h,
                             uint64_t cookie);
/* since spice 0.13.4 */
void spice_qxl_gl_setup(QXLInstance *instance,
                        void *egl_display);

/* since spice 0.14.2 */

/**
 * spice_qxl_set_device_info:
 * @instance the QXL instance to set the path to
 * @device_address the path of the device this QXL instance belongs to
 * @device_display_id_start the starting device display ID of this interface,
 *                          i.e. the device display ID of monitor ID 0
 * @device_display_id_count the total number of device display IDs on this
 *                          interface
 *
 * Sets the device information for this QXL interface, i.e. the hardware
 * address (e.g. PCI) of the graphics device and the IDs of the displays of the
 * graphics device that are exposed by this interface (device display IDs).
 *
 * The supported device address format is:
 * "pci/<DOMAIN>/<SLOT>.<FUNCTION>/.../<SLOT>.<FUNCTION>"
 *
 * The "pci" identifies the rest of the string as a PCI address. It is the only
 * supported address at the moment, other identifiers can be introduced later.
 * <DOMAIN> is the PCI domain, followed by <SLOT>.<FUNCTION> of any PCI bridges
 * in the chain leading to the device. The last <SLOT>.<FUNCTION> is the
 * graphics device. All of <DOMAIN>, <SLOT>, <FUNCTION> are hexadecimal numbers
 * with the following number of digits:
 *   <DOMAIN>: 4
 *   <SLOT>: 2
 *   <FUNCTION>: 1
 *
 * The device_display_id_{start,count} denotes the sequence of device display
 * IDs that map to the zero-based sequence of monitor IDs provided by monitors
 * config on this interface.
 *
 * Example 1:
 *   A QXL graphics device with 3 heads (monitors).
 *
 *   device_display_id_start = 0
 *   device_display_id_count = 3
 *
 *   Results in the following mapping of monitor_id  ->  device_display_id:
 *   0  ->  0
 *   1  ->  1
 *   2  ->  2
 *
 * Example 2:
 *   A virtio graphics device, multiple monitors, a QXL interface for each
 *   monitor. On the QXL interface for the third monitor:
 *
 *   device_display_id_start = 2
 *   device_display_id_count = 1
 *
 *   Results in the following mapping of monitor_id  ->  device_display_id:
 *   0  ->  2
 */
void spice_qxl_set_device_info(QXLInstance *instance,
                               const char *device_address,
                               uint32_t device_display_id_start,
                               uint32_t device_display_id_count);

typedef struct QXLDevInitInfo {
    uint32_t num_memslots_groups;
    uint32_t num_memslots;
    uint8_t memslot_gen_bits;
    uint8_t memslot_id_bits;
    uint32_t qxl_ram_size;
    uint8_t internal_groupslot_id;
    uint32_t n_surfaces;
} QXLDevInitInfo;

struct QXLDevMemSlot {
    uint32_t slot_group_id;
    uint32_t slot_id;
    uint32_t generation;
    unsigned long virt_start;
    unsigned long virt_end;
    uint64_t addr_delta;
    uint32_t qxl_ram_size;
};

struct QXLDevSurfaceCreate {
    uint32_t width;
    uint32_t height;
    int32_t stride;
    uint32_t format;
    uint32_t position;
    uint32_t mouse_mode;
    uint32_t flags;
    uint32_t type;
    uint64_t mem;
    uint32_t group_id;
};

struct QXLInterface {
    SpiceBaseInterface base;

    void (*attache_worker)(QXLInstance *qin, QXLWorker *qxl_worker);
    void (*set_compression_level)(QXLInstance *qin, int level);
    void (*set_mm_time)(QXLInstance *qin, uint32_t mm_time) SPICE_GNUC_DEPRECATED;

    void (*get_init_info)(QXLInstance *qin, QXLDevInitInfo *info);

    /* Retrieve the next command to be processed
     * This call should be non-blocking. If no commands are available, it
     * should return 0, or 1 if a command was retrieved */
    int (*get_command)(QXLInstance *qin, struct QXLCommandExt *cmd);

    /* Request notification when new commands are available
     * When a new command becomes available, the spice server should be
     * notified by calling spice_qxl_wakeup(). If commands are already
     * available, this function should return false and no notification
     * triggered */
    int (*req_cmd_notification)(QXLInstance *qin);
    void (*release_resource)(QXLInstance *qin, struct QXLReleaseInfoExt release_info);
    int (*get_cursor_command)(QXLInstance *qin, struct QXLCommandExt *cmd);
    int (*req_cursor_notification)(QXLInstance *qin);
    void (*notify_update)(QXLInstance *qin, uint32_t update_id);
    int (*flush_resources)(QXLInstance *qin);
    void (*async_complete)(QXLInstance *qin, uint64_t cookie);
    void (*update_area_complete)(QXLInstance *qin, uint32_t surface_id,
                                 struct QXLRect *updated_rects,
                                 uint32_t num_updated_rects);
    /* Available since version 3.2 */
    void (*set_client_capabilities)(QXLInstance *qin,
                                    uint8_t client_present,
                                    uint8_t caps[SPICE_CAPABILITIES_SIZE]);
    /* Returns 1 if the interface is supported, 0 otherwise.
     * if monitors_config is NULL nothing is done except reporting the
     * return code.
     * Available since version 3.3 */
    int (*client_monitors_config)(QXLInstance *qin,
                                  VDAgentMonitorsConfig *monitors_config);
};

struct QXLInstance {
    SpiceBaseInstance  base;
    int                id;
    QXLState           *st;
};

#endif /* SPICE_QXL_H_ */
