/*
   Copyright (C) 2009 Red Hat, Inc.

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

#ifndef _H_REDS
#define _H_REDS

#include <stdint.h>
#include <sys/uio.h>
#include <spice/vd_agent.h>
#include <config.h>


#include "common/marshaller.h"
#include "common/messages.h"
#include "char-device.h"
#include "spice.h"
#include "red-channel.h"
#include "main-dispatcher.h"
#include "migration-protocol.h"

typedef struct RedsState RedsState;

static inline QXLInterface * qxl_get_interface(QXLInstance *qxl)
{
    return SPICE_CONTAINEROF(qxl->base.sif, QXLInterface, base);
}

struct TunnelWorker;
struct SpiceNetWireState {
    struct TunnelWorker *worker;
};

struct SpiceMigrateState {
    int dummy;
};

/* main thread only */
void reds_handle_channel_event(RedsState *reds, int event, SpiceChannelEventInfo *info);

void reds_disable_mm_time(RedsState *reds);
void reds_enable_mm_time(RedsState *reds);
uint32_t reds_get_mm_time(void);
void reds_set_client_mouse_allowed(RedsState *reds,
                                   int is_client_mouse_allowed,
                                   int x_res, int y_res);
void reds_register_channel(RedsState *reds, RedChannel *channel);
void reds_unregister_channel(RedsState *reds, RedChannel *channel);
int reds_get_mouse_mode(RedsState *reds); // used by inputs_channel
gboolean reds_get_agent_mouse(const RedsState *reds); // used by inputs_channel
int reds_has_vdagent(RedsState *reds); // used by inputs channel
void reds_handle_agent_mouse_event(RedsState *reds, const VDAgentMouseState *mouse_state); // used by inputs_channel

GArray* reds_get_renderers(RedsState *reds);

enum {
    RED_RENDERER_INVALID,
    RED_RENDERER_SW,

    RED_RENDERER_LAST
};

// Temporary measures to make splitting reds.c to inputs-channel.c easier

/* should be called only from main_dispatcher */
void reds_client_disconnect(RedsState *reds, RedClient *client);

// Temporary (?) for splitting main channel
typedef struct MainMigrateData MainMigrateData;
void reds_marshall_migrate_data(RedsState *reds, SpiceMarshaller *m);
void reds_fill_channels(RedsState *reds, SpiceMsgChannels *channels_info);
int reds_get_n_channels(RedsState *reds);

/* callbacks from main channel messages */

void reds_on_main_agent_start(RedsState *reds, MainChannelClient *mcc, uint32_t num_tokens);
void reds_on_main_agent_tokens(RedsState *reds, MainChannelClient *mcc, uint32_t num_tokens);
uint8_t *reds_get_agent_data_buffer(RedsState *reds, MainChannelClient *mcc, size_t size);
void reds_release_agent_data_buffer(RedsState *reds, uint8_t *buf);
void reds_on_main_agent_data(RedsState *reds, MainChannelClient *mcc, void *message, size_t size);
void reds_on_main_migrate_connected(RedsState *reds, int seamless); //should be called when all the clients
                                                   // are connected to the target
int reds_handle_migrate_data(RedsState *recs, MainChannelClient *mcc,
                             SpiceMigrateDataMain *mig_data, uint32_t size);
void reds_on_main_mouse_mode_request(RedsState *reds, void *message, size_t size);
/* migration dest side: returns whether it can support seamless migration
 * with the given src migration protocol version */
int reds_on_migrate_dst_set_seamless(RedsState *reds, MainChannelClient *mcc, uint32_t src_version);
void reds_on_client_semi_seamless_migrate_complete(RedsState *reds, RedClient *client);
void reds_on_client_seamless_migrate_complete(RedsState *reds, RedClient *client);
void reds_on_main_channel_migrate(RedsState *reds, MainChannelClient *mcc);
void reds_on_char_device_state_destroy(RedsState *reds, RedCharDevice *dev);

void reds_set_client_mm_time_latency(RedsState *reds, RedClient *client, uint32_t latency);
uint32_t reds_get_streaming_video(const RedsState *reds);
GArray* reds_get_video_codecs(const RedsState *reds);
spice_wan_compression_t reds_get_jpeg_state(const RedsState *reds);
spice_wan_compression_t reds_get_zlib_glz_state(const RedsState *reds);
SpiceCoreInterfaceInternal* reds_get_core_interface(RedsState *reds);
void reds_update_client_mouse_allowed(RedsState *reds);
gboolean reds_use_client_monitors_config(RedsState *reds);
void reds_client_monitors_config(RedsState *reds, VDAgentMonitorsConfig *monitors_config);
MainDispatcher* reds_get_main_dispatcher(RedsState *reds);

/* fd watches/timers */
SpiceWatch *reds_core_watch_add(RedsState *reds,
                                int fd, int event_mask,
                                SpiceWatchFunc func,
                                void *opaque);
void reds_core_watch_update_mask(RedsState *reds,
                                 SpiceWatch *watch,
                                 int event_mask);
void reds_core_watch_remove(RedsState *reds, SpiceWatch *watch);

SpiceTimer *reds_core_timer_add(RedsState *reds,
                                SpiceTimerFunc func,
                                void *opaque);
void reds_core_timer_start(RedsState *reds,
                           SpiceTimer *timer,
                           uint32_t ms);
void reds_core_timer_cancel(RedsState *reds,
                            SpiceTimer *timer);
void reds_core_timer_remove(RedsState *reds,
                            SpiceTimer *timer);

#endif
