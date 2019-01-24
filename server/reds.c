/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <ctype.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>
#else
#include <ws2tcpip.h>
#endif

#include <openssl/err.h>

#if HAVE_SASL
#include <sasl/sasl.h>
#endif

#include <glib.h>

#include <spice/protocol.h>
#include <spice/vd_agent.h>
#include <spice/stats.h>

#include <common/generated_server_marshallers.h>
#include <common/ring.h>

#include "spice.h"
#include "reds.h"
#include "agent-msg-filter.h"
#include "inputs-channel.h"
#include "main-channel.h"
#include "red-qxl.h"
#include "main-dispatcher.h"
#include "sound.h"
#include "stat.h"
#include "char-device.h"
#include "migration-protocol.h"
#ifdef USE_SMARTCARD
#include "smartcard.h"
#endif
#include "red-stream.h"
#include "red-client.h"

#include "reds-private.h"
#include "video-encoder.h"
#include "red-channel-client.h"
#include "main-channel-client.h"
#include "red-client.h"
#include "glib-compat.h"
#include "net-utils.h"
#include "red-stream-device.h"

#define REDS_MAX_STAT_NODES 100

static void reds_client_monitors_config(RedsState *reds, VDAgentMonitorsConfig *monitors_config);
static gboolean reds_use_client_monitors_config(RedsState *reds);
static void reds_set_video_codecs(RedsState *reds, GArray *video_codecs);

static SpiceTimer *adapter_timer_add(const SpiceCoreInterfaceInternal *iface, SpiceTimerFunc func, void *opaque)
{
    return iface->public_interface->timer_add(func, opaque);
}

static void adapter_timer_start(const SpiceCoreInterfaceInternal *iface, SpiceTimer *timer, uint32_t ms)
{
    iface->public_interface->timer_start(timer, ms);
}

static void adapter_timer_cancel(const SpiceCoreInterfaceInternal *iface, SpiceTimer *timer)
{
    iface->public_interface->timer_cancel(timer);
}

static void adapter_timer_remove(const SpiceCoreInterfaceInternal *iface, SpiceTimer *timer)
{
    iface->public_interface->timer_remove(timer);
}

static SpiceWatch *adapter_watch_add(const SpiceCoreInterfaceInternal *iface,
                                     int fd, int event_mask, SpiceWatchFunc func, void *opaque)
{
    return iface->public_interface->watch_add(fd, event_mask, func, opaque);
}

static void adapter_watch_update_mask(const SpiceCoreInterfaceInternal *iface, SpiceWatch *watch, int event_mask)
{
    iface->public_interface->watch_update_mask(watch, event_mask);
}

static void adapter_watch_remove(const SpiceCoreInterfaceInternal *iface, SpiceWatch *watch)
{
    iface->public_interface->watch_remove(watch);
}

static void adapter_channel_event(const SpiceCoreInterfaceInternal *iface, int event, SpiceChannelEventInfo *info)
{
    if (iface->public_interface->base.minor_version >= 3 && iface->public_interface->channel_event != NULL)
        iface->public_interface->channel_event(event, info);
}

static const SpiceCoreInterfaceInternal core_interface_adapter = {
    .timer_add = adapter_timer_add,
    .timer_start = adapter_timer_start,
    .timer_cancel = adapter_timer_cancel,
    .timer_remove = adapter_timer_remove,
    .watch_add = adapter_watch_add,
    .watch_update_mask = adapter_watch_update_mask,
    .watch_remove = adapter_watch_remove,
    .channel_event = adapter_channel_event,
};

/* Debugging only variable: allow multiple client connections to the spice
 * server */
#define SPICE_DEBUG_ALLOW_MC_ENV "SPICE_DEBUG_ALLOW_MC"

#define MIGRATION_NOTIFY_SPICE_KEY "spice_mig_ext"

#define REDS_MIG_VERSION 3
#define REDS_MIG_CONTINUE 1
#define REDS_MIG_ABORT 2
#define REDS_MIG_DIFF_VERSION 3

#define REDS_TOKENS_TO_SEND 5
#define REDS_VDI_PORT_NUM_RECEIVE_BUFFS 5

/* TODO while we can technically create more than one server in a process,
 * the intended use is to support a single server per process */
static GList *servers = NULL;
static pthread_mutex_t global_reds_lock = PTHREAD_MUTEX_INITIALIZER;

/* SPICE configuration set through the public spice_server_set_xxx APIS */
struct RedServerConfig {
    RedsMigSpice *mig_spice;

    int default_channel_security;
    ChannelSecurityOptions *channels_security;

    GArray *renderers;

    int spice_port;
    int spice_secure_port;
    int spice_listen_socket_fd;
    char spice_addr[256];
    int spice_family;
    TicketAuthentication taTicket;

    int sasl_enabled;
#if HAVE_SASL
    char *sasl_appname;
#endif
    char *spice_name;

    bool spice_uuid_is_set;
    uint8_t spice_uuid[16];

    gboolean ticketing_enabled;
    uint32_t streaming_video;
    GArray* video_codecs;
    SpiceImageCompression image_compression;
    bool playback_compression;
    spice_wan_compression_t jpeg_state;
    spice_wan_compression_t zlib_glz_state;

    gboolean agent_mouse;
    gboolean agent_copypaste;
    gboolean agent_file_xfer;
    gboolean exit_on_disconnect;

    RedSSLParameters ssl_parameters;
};


typedef struct RedLinkInfo {
    RedsState *reds;
    RedStream *stream;
    SpiceLinkHeader link_header;
    SpiceLinkMess *link_mess;
    int mess_pos;
    TicketInfo tiTicketing;
    SpiceLinkAuthMechanism auth_mechanism;
    int skip_auth;
} RedLinkInfo;

struct ChannelSecurityOptions {
    uint32_t channel_id;
    uint32_t options;
    ChannelSecurityOptions *next;
};

typedef struct RedVDIReadBuf {
    RedPipeItem base;
    RedCharDeviceVDIPort *dev;

    int len;
    uint8_t data[SPICE_AGENT_MAX_DATA_SIZE];
} RedVDIReadBuf;

typedef enum {
    VDI_PORT_READ_STATE_READ_HEADER,
    VDI_PORT_READ_STATE_GET_BUFF,
    VDI_PORT_READ_STATE_READ_DATA,
} VDIPortReadStates;

struct RedCharDeviceVDIPortPrivate {
    gboolean agent_attached;
    uint32_t plug_generation;
    int client_agent_started;

    /* write to agent */
    RedCharDeviceWriteBuffer *recv_from_client_buf;
    int recv_from_client_buf_pushed;
    AgentMsgFilter write_filter;

    /* read from agent */
    uint32_t num_read_buf;
    VDIPortReadStates read_state;
    uint32_t message_receive_len;
    uint8_t *receive_pos;
    uint32_t receive_len;
    RedVDIReadBuf *current_read_buf;
    AgentMsgFilter read_filter;

    VDIChunkHeader vdi_chunk_header;

    SpiceMigrateDataMain *mig_data; /* storing it when migration data arrives
                                       before agent is attached */
};

/* messages that are addressed to the agent and are created in the server */
typedef struct __attribute__ ((__packed__)) VDInternalBuf {
    VDIChunkHeader chunk_header;
    VDAgentMessage header;
    union {
        VDAgentMouseState mouse_state;
        VDAgentGraphicsDeviceInfo graphics_device_info;
    }
    u;
} VDInternalBuf;

SPICE_DECLARE_TYPE(RedCharDeviceVDIPort, red_char_device_vdi_port, CHAR_DEVICE_VDIPORT);
#define RED_TYPE_CHAR_DEVICE_VDIPORT red_char_device_vdi_port_get_type()

struct RedCharDeviceVDIPort
{
    RedCharDevice parent;

    RedCharDeviceVDIPortPrivate *priv;
};

struct RedCharDeviceVDIPortClass
{
    RedCharDeviceClass parent_class;
};

G_DEFINE_TYPE_WITH_PRIVATE(RedCharDeviceVDIPort, red_char_device_vdi_port, RED_TYPE_CHAR_DEVICE)

static RedCharDeviceVDIPort *red_char_device_vdi_port_new(RedsState *reds);

static void migrate_timeout(void *opaque);
static RedsMigTargetClient* reds_mig_target_client_find(RedsState *reds, RedClient *client);
static void reds_mig_target_client_free(RedsState *reds, RedsMigTargetClient *mig_client);
static void reds_mig_cleanup_wait_disconnect(RedsState *reds);
static void reds_mig_remove_wait_disconnect_client(RedsState *reds, RedClient *client);
static void reds_add_char_device(RedsState *reds, RedCharDevice *dev);
static void reds_send_mm_time(RedsState *reds);
static void reds_on_ic_change(RedsState *reds);
static void reds_on_sv_change(RedsState *reds);
static void reds_on_vc_change(RedsState *reds);
static void reds_on_vm_stop(RedsState *reds);
static void reds_on_vm_start(RedsState *reds);
static void reds_set_mouse_mode(RedsState *reds, SpiceMouseMode mode);
static uint32_t reds_qxl_ram_size(RedsState *reds);
static int calc_compression_level(RedsState *reds);

static RedVDIReadBuf *vdi_port_get_read_buf(RedCharDeviceVDIPort *dev);
static red_pipe_item_free_t vdi_port_read_buf_free;

static ChannelSecurityOptions *reds_find_channel_security(RedsState *reds, int id)
{
    ChannelSecurityOptions *now = reds->config->channels_security;
    while (now && now->channel_id != id) {
        now = now->next;
    }
    return now;
}

void reds_handle_channel_event(RedsState *reds, int event, SpiceChannelEventInfo *info)
{
    reds->core.channel_event(&reds->core, event, info);

    if (event == SPICE_CHANNEL_EVENT_DISCONNECTED) {
        g_free(info);
    }
}

static void reds_link_free(RedLinkInfo *link)
{
    red_stream_free(link->stream);
    link->stream = NULL;

    g_free(link->link_mess);
    link->link_mess = NULL;

    BN_free(link->tiTicketing.bn);
    link->tiTicketing.bn = NULL;

    if (link->tiTicketing.rsa) {
        RSA_free(link->tiTicketing.rsa);
        link->tiTicketing.rsa = NULL;
    }

    g_free(link);
}

#ifdef RED_STATISTICS

void stat_init_node(RedStatNode *node, SpiceServer *reds, const RedStatNode *parent,
                    const char *name, int visible)
{
    StatNodeRef parent_ref = parent ? parent->ref : INVALID_STAT_REF;
    node->ref = stat_file_add_node(reds->stat_file, parent_ref, name, visible);
}

void stat_remove_node(SpiceServer *reds, RedStatNode *node)
{
    if (node->ref != INVALID_STAT_REF) {
        stat_file_remove_node(reds->stat_file, node->ref);
        node->ref = INVALID_STAT_REF;
    }
}

void stat_init_counter(RedStatCounter *counter, SpiceServer *reds,
                       const RedStatNode *parent, const char *name, int visible)
{
    StatNodeRef parent_ref = parent ? parent->ref : INVALID_STAT_REF;
    counter->counter =
        stat_file_add_counter(reds->stat_file, parent_ref, name, visible);
}

void stat_remove_counter(SpiceServer *reds, RedStatCounter *counter)
{
    if (counter->counter) {
        stat_file_remove_counter(reds->stat_file, counter->counter);
        counter->counter = NULL;
    }
}

#endif

void reds_register_channel(RedsState *reds, RedChannel *channel)
{
    spice_assert(reds);
    reds->channels = g_list_prepend(reds->channels, channel);
    // create new channel in the client if possible
    main_channel_registered_new_channel(reds->main_channel, channel);
}

void reds_unregister_channel(RedsState *reds, RedChannel *channel)
{
    reds->channels = g_list_remove(reds->channels, channel);
}

RedChannel *reds_find_channel(RedsState *reds, uint32_t type, uint32_t id)
{
    RedChannel *channel;

    GLIST_FOREACH(reds->channels, RedChannel, channel) {
        uint32_t this_type, this_id;
        g_object_get(channel, "channel-type", &this_type, "id", &this_id, NULL);
        if (this_type == type && this_id == id) {
            return channel;
        }
    }
    return NULL;
}

/* Search for first free channel id for a specific channel type.
 * Return first id free or <0 if not found. */
int reds_get_free_channel_id(RedsState *reds, uint32_t type)
{
    RedChannel *channel;

    // this mark if some IDs are used.
    // The size of the array limits the possible id returned but
    // usually the IDs used for a channel type are not much.
    bool used_ids[256];

    unsigned n;

    // mark id used for the specific channel type
    memset(used_ids, 0, sizeof(used_ids));
    GLIST_FOREACH(reds->channels, RedChannel, channel) {
        uint32_t this_type, this_id;
        g_object_get(channel, "channel-type", &this_type, "id", &this_id, NULL);
        if (this_type == type && this_id < SPICE_N_ELEMENTS(used_ids)) {
            used_ids[this_id] = true;
        }
    }

    // find first ID not marked as used
    for (n = 0; n < SPICE_N_ELEMENTS(used_ids); ++n) {
        if (!used_ids[n]) {
            return n;
        }
    }
    return -1;
}

static void reds_mig_cleanup(RedsState *reds)
{
    if (reds->mig_inprogress) {

        if (reds->mig_wait_connect || reds->mig_wait_disconnect) {
            SpiceMigrateInterface *sif;
            spice_assert(reds->migration_interface);
            sif = SPICE_UPCAST(SpiceMigrateInterface, reds->migration_interface->base.sif);
            if (reds->mig_wait_connect) {
                sif->migrate_connect_complete(reds->migration_interface);
            } else {
                if (sif->migrate_end_complete) {
                    sif->migrate_end_complete(reds->migration_interface);
                }
            }
        }
        reds->mig_inprogress = FALSE;
        reds->mig_wait_connect = FALSE;
        reds->mig_wait_disconnect = FALSE;
        reds_core_timer_cancel(reds, reds->mig_timer);
        reds_mig_cleanup_wait_disconnect(reds);
    }
}

static void reds_reset_vdp(RedsState *reds)
{
    RedCharDeviceVDIPort *dev = reds->agent_dev;
    SpiceCharDeviceInterface *sif;
    RedCharDevice *char_dev;

    dev->priv->read_state = VDI_PORT_READ_STATE_READ_HEADER;
    dev->priv->receive_pos = (uint8_t *)&dev->priv->vdi_chunk_header;
    dev->priv->receive_len = sizeof(dev->priv->vdi_chunk_header);
    dev->priv->message_receive_len = 0;
    if (dev->priv->current_read_buf) {
        red_pipe_item_unref(&dev->priv->current_read_buf->base);
        dev->priv->current_read_buf = NULL;
    }
    /* Reset read filter to start with clean state when the agent reconnects */
    agent_msg_filter_init(&dev->priv->read_filter, reds->config->agent_copypaste,
                          reds->config->agent_file_xfer,
                          reds_use_client_monitors_config(reds), TRUE);
    /* Throw away pending chunks from the current (if any) and future
     * messages written by the client.
     * TODO: client should clear its agent messages queue when the agent
     * is disconnected. Currently, when an agent gets disconnected and reconnected,
     * messages that were directed to the previous instance of the agent continue
     * to be sent from the client. This TODO will require server, protocol, and client changes */
    dev->priv->write_filter.result = AGENT_MSG_FILTER_DISCARD;
    dev->priv->write_filter.discard_all = TRUE;
    dev->priv->client_agent_started = FALSE;

    /*  The client's tokens are set once when the main channel is initialized
     *  and once upon agent's connection with SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS.
     *  The client tokens are tracked as part of the RedCharDeviceClient. Thus,
     *  in order to be backward compatible with the client, we need to track the tokens
     *  even if the agent is detached. We don't destroy the char_device, and
     *  instead we just reset it.
     *  The tokens are also reset to avoid mismatch in upon agent reconnection.
     */
    dev->priv->agent_attached = FALSE;
    char_dev = RED_CHAR_DEVICE(dev);
    red_char_device_stop(char_dev);
    red_char_device_reset(char_dev);
    red_char_device_reset_dev_instance(char_dev, NULL);

    sif = spice_char_device_get_interface(reds->vdagent);
    if (sif->state) {
        sif->state(reds->vdagent, 0);
    }
}

static RedCharDeviceWriteBuffer *vdagent_new_write_buffer(RedCharDeviceVDIPort *agent_dev,
                                                          uint32_t type,
                                                          size_t size,
                                                          bool use_token)
{
    uint32_t total_msg_size = sizeof(VDIChunkHeader) + sizeof(VDAgentMessage) + size;

    RedCharDeviceWriteBuffer *char_dev_buf;
        char_dev_buf = red_char_device_write_buffer_get_server(RED_CHAR_DEVICE(agent_dev),
                                                               total_msg_size,
                                                               use_token);
    if (!char_dev_buf) {
        return NULL;  // no token was available
    }

    char_dev_buf->buf_used = total_msg_size;
    VDInternalBuf *internal_buf = (VDInternalBuf *)char_dev_buf->buf;
    internal_buf->chunk_header.port = VDP_SERVER_PORT;
    internal_buf->chunk_header.size = sizeof(VDAgentMessage) + size;
    internal_buf->header.protocol = VD_AGENT_PROTOCOL;
    internal_buf->header.type = type;
    internal_buf->header.opaque = 0;
    internal_buf->header.size = size;

    return char_dev_buf;
}

static int reds_main_channel_connected(RedsState *reds)
{
    return main_channel_is_connected(reds->main_channel);
}

void reds_client_disconnect(RedsState *reds, RedClient *client)
{
    RedsMigTargetClient *mig_client;

    if (reds->config->exit_on_disconnect)
    {
        spice_debug("Exiting server because of client disconnect.");
        exit(0);
    }

    if (!client || red_client_is_disconnecting(client)) {
        spice_debug("client %p already during disconnection", client);
        return;
    }

    spice_debug("trace");
    /* disconnecting is set to prevent recursion because of the following:
     * main_channel_client_on_disconnect->
     *  reds_client_disconnect->red_client_destroy->main_channel...
     */
    red_client_set_disconnecting(client);

    // TODO: we need to handle agent properly for all clients!!!! (e.g., cut and paste, how?)
    // We shouldn't initialize the agent when there are still clients connected

    mig_client = reds_mig_target_client_find(reds, client);
    if (mig_client) {
        reds_mig_target_client_free(reds, mig_client);
    }

    if (reds->mig_wait_disconnect) {
        reds_mig_remove_wait_disconnect_client(reds, client);
    }

    /* note that client might be NULL, if the vdagent was once
     * up and than was removed */
    if (red_char_device_client_exists(RED_CHAR_DEVICE(reds->agent_dev), client)) {
        red_char_device_client_remove(RED_CHAR_DEVICE(reds->agent_dev), client);
    }

    reds->clients = g_list_remove(reds->clients, client);
    red_client_destroy(client);

   // TODO: we need to handle agent properly for all clients!!!! (e.g., cut and paste, how? Maybe throw away messages
   // if we are in the middle of one from another client)
    if (g_list_length(reds->clients) == 0) {
        /* Let the agent know the client is disconnected */
        if (reds->agent_dev->priv->agent_attached) {
            RedCharDeviceWriteBuffer *char_dev_buf =
                vdagent_new_write_buffer(reds->agent_dev,
                                         VD_AGENT_CLIENT_DISCONNECTED,
                                         0,
                                         false);

            red_char_device_write_buffer_add(RED_CHAR_DEVICE(reds->agent_dev), char_dev_buf);
        }

        /* Reset write filter to start with clean state on client reconnect */
        agent_msg_filter_init(&reds->agent_dev->priv->write_filter, reds->config->agent_copypaste,
                              reds->config->agent_file_xfer,
                              reds_use_client_monitors_config(reds), TRUE);

        /* Throw away pending chunks from the current (if any) and future
         *  messages read from the agent */
        reds->agent_dev->priv->read_filter.result = AGENT_MSG_FILTER_DISCARD;
        reds->agent_dev->priv->read_filter.discard_all = TRUE;
        g_free(reds->agent_dev->priv->mig_data);
        reds->agent_dev->priv->mig_data = NULL;

        reds_mig_cleanup(reds);
    }
}

// TODO: go over all usage of reds_disconnect, most/some of it should be converted to
// reds_client_disconnect
static void reds_disconnect(RedsState *reds)
{
    RedClient *client;

    spice_debug("trace");
    GLIST_FOREACH(reds->clients, RedClient, client) {
        reds_client_disconnect(reds, client);
    }
    reds_mig_cleanup(reds);
}

static void reds_mig_disconnect(RedsState *reds)
{
    if (reds_main_channel_connected(reds)) {
        reds_disconnect(reds);
    } else {
        reds_mig_cleanup(reds);
    }
}

bool reds_config_get_playback_compression(RedsState *reds)
{
    return reds->config->playback_compression;
}

SpiceMouseMode reds_get_mouse_mode(RedsState *reds)
{
    return reds->mouse_mode;
}

static void reds_set_mouse_mode(RedsState *reds, SpiceMouseMode mode)
{
    QXLInstance *qxl;

    if (reds->mouse_mode == mode) {
        return;
    }
    reds->mouse_mode = mode;

    FOREACH_QXL_INSTANCE(reds, qxl) {
        red_qxl_set_mouse_mode(qxl, mode);
    }

    main_channel_push_mouse_mode(reds->main_channel, reds->mouse_mode, reds->is_client_mouse_allowed);
}

gboolean reds_config_get_agent_mouse(const RedsState *reds)
{
    return reds->config->agent_mouse;
}

static void reds_update_mouse_mode(RedsState *reds)
{
    int allowed = 0;
    int qxl_count = g_list_length(reds->qxl_instances);
    int display_channel_count = 0;
    RedChannel *channel;

    GLIST_FOREACH(reds->channels, RedChannel, channel) {
        uint32_t this_type;
        g_object_get(channel, "channel-type", &this_type, NULL);
        if (this_type == SPICE_CHANNEL_DISPLAY) {
            ++display_channel_count;
        }
    }

    if ((reds->config->agent_mouse && reds->vdagent) ||
        (inputs_channel_has_tablet(reds->inputs_channel) &&
            qxl_count == 1 && display_channel_count == 1)) {
        allowed = reds->dispatcher_allows_client_mouse;
    }
    if (allowed == reds->is_client_mouse_allowed) {
        return;
    }
    reds->is_client_mouse_allowed = allowed;
    if (reds->mouse_mode == SPICE_MOUSE_MODE_CLIENT && !allowed) {
        reds_set_mouse_mode(reds, SPICE_MOUSE_MODE_SERVER);
        return;
    }
    if (reds->main_channel) {
        main_channel_push_mouse_mode(reds->main_channel, reds->mouse_mode,
                                     reds->is_client_mouse_allowed);
    }
}

static void reds_update_agent_properties(RedsState *reds)
{
    if (reds->agent_dev == NULL || reds->config == NULL) {
        return;
    }
    /* copy & paste */
    reds->agent_dev->priv->write_filter.copy_paste_enabled = reds->config->agent_copypaste;
    reds->agent_dev->priv->read_filter.copy_paste_enabled = reds->config->agent_copypaste;
    /* file transfer */
    reds->agent_dev->priv->write_filter.file_xfer_enabled = reds->config->agent_file_xfer;
    reds->agent_dev->priv->read_filter.file_xfer_enabled = reds->config->agent_file_xfer;
}

static void reds_agent_remove(RedsState *reds)
{
    // TODO: agent is broken with multiple clients. also need to figure out what to do when
    // part of the clients are during target migration.
    reds_reset_vdp(reds);

    reds->vdagent = NULL;
    reds_update_mouse_mode(reds);
    if (reds_main_channel_connected(reds) &&
        !red_channel_is_waiting_for_migrate_data(RED_CHANNEL(reds->main_channel))) {
        main_channel_push_agent_disconnected(reds->main_channel);
    }
}

static void vdi_port_read_buf_release(uint8_t *data, void *opaque)
{
    RedVDIReadBuf *read_buf = (RedVDIReadBuf *)opaque;
    red_pipe_item_unref(&read_buf->base);
}

/*
    returns the #AgentMsgFilterResult value:
        AGENT_MSG_FILTER_OK if the buffer can be forwarded,
        AGENT_MSG_FILTER_PROTO_ERROR on error
        other values can be discarded
*/
static AgentMsgFilterResult vdi_port_read_buf_process(RedCharDeviceVDIPort *dev,
                                                      RedVDIReadBuf *buf)
{
    switch (dev->priv->vdi_chunk_header.port) {
    case VDP_CLIENT_PORT:
        return agent_msg_filter_process_data(&dev->priv->read_filter, buf->data, buf->len);
    case VDP_SERVER_PORT:
        return AGENT_MSG_FILTER_DISCARD;
    default:
        spice_warning("invalid port");
        return AGENT_MSG_FILTER_PROTO_ERROR;
    }
}

static RedVDIReadBuf *vdi_read_buf_new(RedCharDeviceVDIPort *dev)
{
    RedVDIReadBuf *buf = g_new(RedVDIReadBuf, 1);

    /* Bogus pipe item type, we only need the RingItem and refcounting
     * from the base class and are not going to use the type
     */
    red_pipe_item_init_full(&buf->base, -1,
                            vdi_port_read_buf_free);
    buf->dev = dev;
    buf->len = 0;
    return buf;
}

static RedVDIReadBuf *vdi_port_get_read_buf(RedCharDeviceVDIPort *dev)
{
    if (dev->priv->num_read_buf >= REDS_VDI_PORT_NUM_RECEIVE_BUFFS) {
        return NULL;
    }

    dev->priv->num_read_buf++;
    return vdi_read_buf_new(dev);
}

static void vdi_port_read_buf_free(RedPipeItem *base)
{
    RedVDIReadBuf *buf = SPICE_UPCAST(RedVDIReadBuf, base);

    g_warn_if_fail(buf->base.refcount == 0);
    buf->dev->priv->num_read_buf--;

    /* read_one_msg_from_vdi_port may have never completed because we
       reached buffer limit. So we call it again so it can complete its work if
       necessary. Note that since we can be called from red_char_device_wakeup
       this can cause recursion, but we have protection for that */
    if (buf->dev->priv->agent_attached) {
       red_char_device_wakeup(RED_CHAR_DEVICE(buf->dev));
    }
    g_free(buf);
}

static void agent_adjust_capabilities(VDAgentMessage *message,
                                      bool clipboard_enabled, bool xfer_enabled)
{
    VDAgentAnnounceCapabilities *capabilities;

    if (message->type != VD_AGENT_ANNOUNCE_CAPABILITIES) {
        return;
    }
    capabilities = (VDAgentAnnounceCapabilities *) message->data;

    if (!clipboard_enabled) {
        VD_AGENT_CLEAR_CAPABILITY(capabilities->caps, VD_AGENT_CAP_CLIPBOARD);
        VD_AGENT_CLEAR_CAPABILITY(capabilities->caps, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
        VD_AGENT_CLEAR_CAPABILITY(capabilities->caps, VD_AGENT_CAP_CLIPBOARD_SELECTION);
    }

    if (!xfer_enabled) {
        VD_AGENT_SET_CAPABILITY(capabilities->caps, VD_AGENT_CAP_FILE_XFER_DISABLED);
    }
}

/* reads from the device till completes reading a message that is addressed to the client,
 * or otherwise, when reading from the device fails */
static RedPipeItem *vdi_port_read_one_msg_from_device(RedCharDevice *self,
                                                      SpiceCharDeviceInstance *sin)
{
    RedsState *reds;
    RedCharDeviceVDIPort *dev = RED_CHAR_DEVICE_VDIPORT(self);
    SpiceCharDeviceInterface *sif;
    RedVDIReadBuf *dispatch_buf;
    int n;

    reds = red_char_device_get_server(self);
    g_assert(RED_CHAR_DEVICE(reds->agent_dev) == sin->st);
    if (!reds->vdagent) {
        return NULL;
    }
    spice_assert(reds->vdagent == sin);
    sif = spice_char_device_get_interface(reds->vdagent);
    while (reds->vdagent) {
        switch (dev->priv->read_state) {
        case VDI_PORT_READ_STATE_READ_HEADER:
            n = sif->read(reds->vdagent, dev->priv->receive_pos, dev->priv->receive_len);
            if (!n) {
                return NULL;
            }
            if ((dev->priv->receive_len -= n)) {
                dev->priv->receive_pos += n;
                return NULL;
            }
            dev->priv->message_receive_len = dev->priv->vdi_chunk_header.size;
            dev->priv->read_state = VDI_PORT_READ_STATE_GET_BUFF;
            /* fall through */
        case VDI_PORT_READ_STATE_GET_BUFF: {
            if (!(dev->priv->current_read_buf = vdi_port_get_read_buf(dev))) {
                return NULL;
            }
            dev->priv->receive_pos = dev->priv->current_read_buf->data;
            dev->priv->receive_len = MIN(dev->priv->message_receive_len,
                                         sizeof(dev->priv->current_read_buf->data));
            dev->priv->current_read_buf->len = dev->priv->receive_len;
            dev->priv->message_receive_len -= dev->priv->receive_len;
            dev->priv->read_state = VDI_PORT_READ_STATE_READ_DATA;
        }
            /* fall through */
        case VDI_PORT_READ_STATE_READ_DATA: {
            n = sif->read(reds->vdagent, dev->priv->receive_pos, dev->priv->receive_len);
            if (!n) {
                return NULL;
            }
            if ((dev->priv->receive_len -= n)) {
                dev->priv->receive_pos += n;
                break;
            }
            dispatch_buf = dev->priv->current_read_buf;
            dev->priv->current_read_buf = NULL;
            dev->priv->receive_pos = NULL;
            if (dev->priv->message_receive_len == 0) {
                dev->priv->read_state = VDI_PORT_READ_STATE_READ_HEADER;
                dev->priv->receive_pos = (uint8_t *)&dev->priv->vdi_chunk_header;
                dev->priv->receive_len = sizeof(dev->priv->vdi_chunk_header);
            } else {
                dev->priv->read_state = VDI_PORT_READ_STATE_GET_BUFF;
            }
            switch (vdi_port_read_buf_process(dev, dispatch_buf)) {
            case AGENT_MSG_FILTER_OK:
                agent_adjust_capabilities((VDAgentMessage *) dispatch_buf->data,
                                          reds->config->agent_copypaste,
                                          reds->config->agent_file_xfer);
                return &dispatch_buf->base;
            case AGENT_MSG_FILTER_PROTO_ERROR:
                reds_agent_remove(reds);
                /* fall through */
            case AGENT_MSG_FILTER_MONITORS_CONFIG:
                /* fall through */
            case AGENT_MSG_FILTER_DISCARD:
                red_pipe_item_unref(&dispatch_buf->base);
            }
        }
        } /* END switch */
    } /* END while */
    return NULL;
}

void reds_marshall_device_display_info(RedsState *reds, SpiceMarshaller *m)
{
    QXLInstance *qxl;
    RedCharDevice *dev;

    uint32_t device_count = 0;
    void *device_count_ptr = spice_marshaller_add_uint32(m, device_count);

    // add the qxl devices to the message
    FOREACH_QXL_INSTANCE(reds, qxl) {
        const char *const device_address = red_qxl_get_device_address(qxl);
        const size_t device_address_len = strlen(device_address) + 1;
        if (device_address_len == 1) {
            continue;
        }
        for (size_t i = 0; i < red_qxl_get_monitors_count(qxl); ++i) {
            spice_marshaller_add_uint32(m, qxl->id);
            spice_marshaller_add_uint32(m, i);
            spice_marshaller_add_uint32(m, red_qxl_get_device_display_ids(qxl)[i]);
            spice_marshaller_add_uint32(m, device_address_len);
            spice_marshaller_add(m, (void*) device_address, device_address_len);
            ++device_count;

            g_debug("   (qxl)    channel_id: %u monitor_id: %zu, device_address: %s, "
                    "device_display_id: %u",
                    qxl->id, i, device_address,
                    red_qxl_get_device_display_ids(qxl)[i]);
        }
    }

    // add the stream devices to the message
    GLIST_FOREACH(reds->char_devices, RedCharDevice, dev) {
        if (IS_STREAM_DEVICE(dev)) {
            StreamDevice *stream_dev = STREAM_DEVICE(dev);
            const StreamDeviceDisplayInfo *info = stream_device_get_device_display_info(stream_dev);
            size_t device_address_len = strlen(info->device_address) + 1;

            if (device_address_len == 1) {
                // the device info wasn't set (yet), don't send it
                continue;
            }

            int32_t channel_id = stream_device_get_stream_channel_id(stream_dev);
            if (channel_id == -1) {
                g_warning("DeviceDisplayInfo set but no stream channel exists");
                continue;
            }

            spice_marshaller_add_uint32(m, channel_id);
            spice_marshaller_add_uint32(m, info->stream_id);
            spice_marshaller_add_uint32(m, info->device_display_id);
            spice_marshaller_add_uint32(m, device_address_len);
            spice_marshaller_add(m, (void*) info->device_address, device_address_len);
            ++device_count;

            g_debug("   (stream) channel_id: %u monitor_id: %u, device_address: %s, "
                    "device_display_id: %u",
                    channel_id, info->stream_id, info->device_address,
                    info->device_display_id);
        }
    }
    spice_marshaller_set_uint32(m, device_count_ptr, device_count);
}

void reds_send_device_display_info(RedsState *reds)
{
    if (!reds->agent_dev->priv->agent_attached) {
        return;
    }
    g_debug("Sending device display info to the agent:");

    SpiceMarshaller *m = spice_marshaller_new();
    reds_marshall_device_display_info(reds, m);

    RedCharDeviceWriteBuffer *char_dev_buf = vdagent_new_write_buffer(reds->agent_dev,
                                         VD_AGENT_GRAPHICS_DEVICE_INFO,
                                         spice_marshaller_get_total_size(m),
                                         true);

    if (!char_dev_buf) {
        spice_marshaller_destroy(m);
        reds->pending_device_display_info_message = true;
        return;
    }

    VDInternalBuf *internal_buf = (VDInternalBuf *)char_dev_buf->buf;

    int free_info;
    size_t len_info;
    uint8_t *info = spice_marshaller_linearize(m, 0, &len_info, &free_info);
    memcpy(&internal_buf->u.graphics_device_info, info, len_info);
    if (free_info) {
        free(info);
    }
    spice_marshaller_destroy(m);

    reds->pending_device_display_info_message = false;

    red_char_device_write_buffer_add(RED_CHAR_DEVICE(reds->agent_dev), char_dev_buf);
}

/* after calling this, we unref the message, and the ref is in the instance side */
static void vdi_port_send_msg_to_client(RedCharDevice *self,
                                        RedPipeItem *msg,
                                        RedClient *client)
{
    RedVDIReadBuf *agent_data_buf = (RedVDIReadBuf *)msg;

    red_pipe_item_ref(msg);
    main_channel_client_push_agent_data(red_client_get_main(client),
                                        agent_data_buf->data,
                                        agent_data_buf->len,
                                        vdi_port_read_buf_release,
                                        agent_data_buf);
}

static void vdi_port_send_tokens_to_client(RedCharDevice *self,
                                           RedClient *client,
                                           uint32_t tokens)
{
    main_channel_client_push_agent_tokens(red_client_get_main(client),
                                          tokens);
}

static void vdi_port_on_free_self_token(RedCharDevice *self)
{
    RedsState *reds = red_char_device_get_server(self);

    if (reds->inputs_channel && reds->pending_mouse_event) {
        spice_debug("pending mouse event");
        reds_handle_agent_mouse_event(reds, inputs_channel_get_mouse_state(reds->inputs_channel));
    }

    if (reds->pending_device_display_info_message) {
        spice_debug("pending device display info message");
        reds_send_device_display_info(reds);
    }
}

static void vdi_port_remove_client(RedCharDevice *self,
                                   RedClient *client)
{
    red_channel_client_shutdown(RED_CHANNEL_CLIENT(red_client_get_main(client)));
}

/****************************************************************************/

int reds_has_vdagent(RedsState *reds)
{
    return !!reds->vdagent;
}

void reds_handle_agent_mouse_event(RedsState *reds, const VDAgentMouseState *mouse_state)
{
    if (!reds->inputs_channel || !reds->agent_dev->priv->agent_attached) {
        return;
    }

    RedCharDeviceWriteBuffer *char_dev_buf = vdagent_new_write_buffer(reds->agent_dev,
                                                                      VD_AGENT_MOUSE_STATE,
                                                                      sizeof(VDAgentMouseState),
                                                                      true);

    if (!char_dev_buf) {
        reds->pending_mouse_event = TRUE;
        return;
    }

    reds->pending_mouse_event = FALSE;

    VDInternalBuf *internal_buf = (VDInternalBuf *)char_dev_buf->buf;
    internal_buf->u.mouse_state = *mouse_state;

    red_char_device_write_buffer_add(RED_CHAR_DEVICE(reds->agent_dev), char_dev_buf);
}

SPICE_GNUC_VISIBLE int spice_server_get_num_clients(SpiceServer *reds)
{
    return reds ? g_list_length(reds->clients) : 0;
}

static bool channel_supports_multiple_clients(RedChannel *channel)
{
    uint32_t type;
    g_object_get(channel, "channel-type", &type, NULL);
    switch (type) {
    case SPICE_CHANNEL_MAIN:
    case SPICE_CHANNEL_DISPLAY:
    case SPICE_CHANNEL_CURSOR:
    case SPICE_CHANNEL_INPUTS:
        return TRUE;
    }
    return FALSE;
}

static void reds_fill_channels(RedsState *reds, SpiceMsgChannels *channels_info)
{
    RedChannel *channel;
    int used_channels = 0;

    GLIST_FOREACH(reds->channels, RedChannel, channel) {
        uint32_t type, id;
        if (g_list_length(reds->clients) > 1 &&
            !channel_supports_multiple_clients(channel)) {
            continue;
        }
        g_object_get(channel, "channel-type", &type, "id", &id, NULL);
        channels_info->channels[used_channels].type = type;
        channels_info->channels[used_channels].id = id;
        used_channels++;
    }

    channels_info->num_of_channels = used_channels;
    if (used_channels != g_list_length(reds->channels)) {
        spice_warning("sent %d out of %d", used_channels, g_list_length(reds->channels));
    }
}

SpiceMsgChannels *reds_msg_channels_new(RedsState *reds)
{
    SpiceMsgChannels* channels_info;

    spice_assert(reds != NULL);

    channels_info = (SpiceMsgChannels *)g_malloc(sizeof(SpiceMsgChannels)
                            + g_list_length(reds->channels) * sizeof(SpiceChannelId));

    reds_fill_channels(reds, channels_info);

    return channels_info;
}

void reds_on_main_agent_start(RedsState *reds, MainChannelClient *mcc, uint32_t num_tokens)
{
    RedCharDevice *dev_state = RED_CHAR_DEVICE(reds->agent_dev);
    RedChannelClient *rcc;
    RedClient *client;

    if (!reds->vdagent) {
        return;
    }
    spice_assert(reds->vdagent->st && reds->vdagent->st == dev_state);
    rcc = RED_CHANNEL_CLIENT(mcc);
    client = red_channel_client_get_client(rcc);
    reds->agent_dev->priv->client_agent_started = TRUE;
    /*
     * Note that in older releases, send_tokens were set to ~0 on both client
     * and server. The server ignored the client given tokens.
     * Thanks to that, when an old client is connected to a new server,
     * and vice versa, the sending from the server to the client won't have
     * flow control, but will have no other problem.
     */
    if (!red_char_device_client_exists(dev_state, client)) {
        int client_added;

        client_added = red_char_device_client_add(dev_state,
                                                  client,
                                                  TRUE, /* flow control */
                                                  REDS_VDI_PORT_NUM_RECEIVE_BUFFS,
                                                  REDS_AGENT_WINDOW_SIZE,
                                                  num_tokens,
                                                  red_channel_client_is_waiting_for_migrate_data(rcc));

        if (!client_added) {
            spice_warning("failed to add client to agent");
            red_channel_client_shutdown(rcc);
            return;
        }
    } else {
        red_char_device_send_to_client_tokens_set(dev_state,
                                                  client,
                                                  num_tokens);
    }

    reds_send_device_display_info(reds);

    agent_msg_filter_config(&reds->agent_dev->priv->write_filter, reds->config->agent_copypaste,
                            reds->config->agent_file_xfer,
                            reds_use_client_monitors_config(reds));
    reds->agent_dev->priv->write_filter.discard_all = FALSE;
}

void reds_on_main_agent_tokens(RedsState *reds, MainChannelClient *mcc, uint32_t num_tokens)
{
    RedClient *client = red_channel_client_get_client(RED_CHANNEL_CLIENT(mcc));
    if (!reds->vdagent) {
        return;
    }
    spice_assert(reds->vdagent->st);
    red_char_device_send_to_client_tokens_add(reds->vdagent->st,
                                                client,
                                                num_tokens);
}

uint8_t *reds_get_agent_data_buffer(RedsState *reds, MainChannelClient *mcc, size_t size)
{
    RedCharDeviceVDIPort *dev = reds->agent_dev;
    RedClient *client;

    if (!dev->priv->client_agent_started) {
        /*
         * agent got disconnected, and possibly got reconnected, but we still can receive
         * msgs that are addressed to the agent's old instance, in case they were
         * sent by the client before it received the AGENT_DISCONNECTED msg.
         * In such case, we will receive and discard the msgs (reds_reset_vdp takes care
         * of setting dev->write_filter.result = AGENT_MSG_FILTER_DISCARD).
         */
        return g_malloc(size);
    }

    spice_assert(dev->priv->recv_from_client_buf == NULL);
    client = red_channel_client_get_client(RED_CHANNEL_CLIENT(mcc));
    dev->priv->recv_from_client_buf =
        red_char_device_write_buffer_get_client(RED_CHAR_DEVICE(dev),
                                                client,
                                                size + sizeof(VDIChunkHeader));
    /* check if buffer was allocated, as flow control is enabled for
     * this device this is a normal condition */
    if (!dev->priv->recv_from_client_buf) {
        return NULL;
    }
    dev->priv->recv_from_client_buf_pushed = FALSE;
    return dev->priv->recv_from_client_buf->buf + sizeof(VDIChunkHeader);
}

void reds_release_agent_data_buffer(RedsState *reds, uint8_t *buf)
{
    RedCharDeviceVDIPort *dev = reds->agent_dev;

    if (!dev->priv->recv_from_client_buf) {
        g_free(buf);
        return;
    }

    spice_assert(buf == dev->priv->recv_from_client_buf->buf + sizeof(VDIChunkHeader));
    /* if we pushed the buffer the buffer is attached to the channel so don't free it */
    if (!dev->priv->recv_from_client_buf_pushed) {
        red_char_device_write_buffer_release(RED_CHAR_DEVICE(dev),
                                             &dev->priv->recv_from_client_buf);
    }
    dev->priv->recv_from_client_buf = NULL;
    dev->priv->recv_from_client_buf_pushed = FALSE;
}

static void reds_on_main_agent_monitors_config(RedsState *reds,
        MainChannelClient *mcc, const void *message, size_t size)
{
    const unsigned int MAX_MONITORS = 256;
    const unsigned int MAX_MONITOR_CONFIG_SIZE =
       sizeof(VDAgentMonitorsConfig) + MAX_MONITORS * sizeof(VDAgentMonConfig);

    VDAgentMessage *msg_header;
    VDAgentMonitorsConfig *monitors_config;
    SpiceBuffer *cmc = &reds->client_monitors_config;
    uint32_t max_monitors;

    // limit size of message sent by the client as this can cause a DoS through
    // memory exhaustion, or potentially some integer overflows
    if (sizeof(VDAgentMessage) + MAX_MONITOR_CONFIG_SIZE - cmc->offset < size) {
        goto overflow;
    }
    spice_buffer_append(cmc, message, size);
    if (sizeof(VDAgentMessage) > cmc->offset) {
        spice_debug("not enough data yet. %zd", cmc->offset);
        return;
    }
    msg_header = (VDAgentMessage *)cmc->buffer;
    if (msg_header->size > MAX_MONITOR_CONFIG_SIZE) {
        goto overflow;
    }
    if (msg_header->size > cmc->offset - sizeof(VDAgentMessage)) {
        spice_debug("not enough data yet. %zd", cmc->offset);
        return;
    }
    if (msg_header->size < sizeof(VDAgentMonitorsConfig)) {
        goto overflow;
    }
    monitors_config = (VDAgentMonitorsConfig *)(cmc->buffer + sizeof(*msg_header));
    // limit the monitor number to avoid buffer overflows
    max_monitors = (msg_header->size - sizeof(VDAgentMonitorsConfig)) /
                   sizeof(VDAgentMonConfig);
    if (monitors_config->num_of_monitors > max_monitors) {
        goto overflow;
    }
    spice_debug("monitors_config->num_of_monitors: %d", monitors_config->num_of_monitors);
    reds_client_monitors_config(reds, monitors_config);
    spice_buffer_free(cmc);
    return;

overflow:
    spice_warning("received invalid MonitorsConfig request from client, disconnecting");
    red_channel_client_disconnect(RED_CHANNEL_CLIENT(mcc));
    spice_buffer_free(cmc);
}

void reds_on_main_agent_data(RedsState *reds, MainChannelClient *mcc, const void *message,
                             size_t size)
{
    RedCharDeviceVDIPort *dev = reds->agent_dev;
    VDIChunkHeader *header;
    AgentMsgFilterResult res;

    res = agent_msg_filter_process_data(&dev->priv->write_filter,
                                        message, size);
    switch (res) {
    case AGENT_MSG_FILTER_OK:
        break;
    case AGENT_MSG_FILTER_DISCARD:
        return;
    case AGENT_MSG_FILTER_MONITORS_CONFIG:
        reds_on_main_agent_monitors_config(reds, mcc, message, size);
        return;
    case AGENT_MSG_FILTER_PROTO_ERROR:
        red_channel_client_shutdown(RED_CHANNEL_CLIENT(mcc));
        return;
    }

    spice_assert(dev->priv->recv_from_client_buf);
    spice_assert(message == dev->priv->recv_from_client_buf->buf + sizeof(VDIChunkHeader));
    // TODO - start tracking agent data per channel
    header =  (VDIChunkHeader *)dev->priv->recv_from_client_buf->buf;
    header->port = VDP_CLIENT_PORT;
    header->size = size;
    dev->priv->recv_from_client_buf->buf_used = sizeof(VDIChunkHeader) + size;

    dev->priv->recv_from_client_buf_pushed = TRUE;
    red_char_device_write_buffer_add(RED_CHAR_DEVICE(dev), dev->priv->recv_from_client_buf);
}

void reds_on_main_migrate_connected(RedsState *reds, int seamless)
{
    reds->src_do_seamless_migrate = seamless;
    if (reds->mig_wait_connect) {
        reds_mig_cleanup(reds);
    }
}

void reds_on_main_mouse_mode_request(RedsState *reds, void *message, size_t size)
{
    switch (((SpiceMsgcMainMouseModeRequest *)message)->mode) {
    case SPICE_MOUSE_MODE_CLIENT:
        if (reds->is_client_mouse_allowed) {
            reds_set_mouse_mode(reds, SPICE_MOUSE_MODE_CLIENT);
        } else {
            spice_debug("client mouse is disabled");
        }
        break;
    case SPICE_MOUSE_MODE_SERVER:
        reds_set_mouse_mode(reds, SPICE_MOUSE_MODE_SERVER);
        break;
    default:
        spice_warning("unsupported mouse mode");
    }
}

/*
 * Push partial agent data, even if not all the chunk was consumend,
 * in order to avoid the roundtrip (src-server->client->dest-server)
 */
void reds_on_main_channel_migrate(RedsState *reds, MainChannelClient *mcc)
{
    RedCharDeviceVDIPort *agent_dev = reds->agent_dev;
    uint32_t read_data_len;

    spice_assert(g_list_length(reds->clients) == 1);

    if (agent_dev->priv->read_state != VDI_PORT_READ_STATE_READ_DATA) {
        return;
    }
    spice_assert(agent_dev->priv->current_read_buf &&
                 agent_dev->priv->receive_pos > agent_dev->priv->current_read_buf->data);
    read_data_len = agent_dev->priv->receive_pos - agent_dev->priv->current_read_buf->data;

    if (agent_dev->priv->read_filter.msg_data_to_read ||
        read_data_len > sizeof(VDAgentMessage)) { /* msg header has been read */
        RedVDIReadBuf *read_buf = agent_dev->priv->current_read_buf;

        spice_debug("push partial read %u (msg first chunk? %d)", read_data_len,
                    !agent_dev->priv->read_filter.msg_data_to_read);

        read_buf->len = read_data_len;
        switch (vdi_port_read_buf_process(agent_dev, read_buf)) {
        case AGENT_MSG_FILTER_OK:
            agent_adjust_capabilities((VDAgentMessage *)read_buf->data,
                                      reds->config->agent_copypaste,
                                      reds->config->agent_file_xfer);
            main_channel_client_push_agent_data(mcc,
                                                read_buf->data,
                                                read_buf->len,
                                                vdi_port_read_buf_release,
                                                read_buf);
            break;
        case AGENT_MSG_FILTER_PROTO_ERROR:
            reds_agent_remove(reds);
            /* fall through */
        case AGENT_MSG_FILTER_MONITORS_CONFIG:
            /* fall through */
        case AGENT_MSG_FILTER_DISCARD:
            red_pipe_item_unref(&read_buf->base);
        }

        spice_assert(agent_dev->priv->receive_len);
        agent_dev->priv->message_receive_len += agent_dev->priv->receive_len;
        agent_dev->priv->read_state = VDI_PORT_READ_STATE_GET_BUFF;
        agent_dev->priv->current_read_buf = NULL;
        agent_dev->priv->receive_pos = NULL;
    }
}

void reds_marshall_migrate_data(RedsState *reds, SpiceMarshaller *m)
{
    SpiceMigrateDataMain mig_data;
    RedCharDeviceVDIPort *agent_dev = reds->agent_dev;
    SpiceMarshaller *m2;

    memset(&mig_data, 0, sizeof(mig_data));
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_MAIN_MAGIC);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_MAIN_VERSION);

    if (!reds->vdagent) {
        uint8_t *null_agent_mig_data;

        /* MSG_AGENT_CONNECTED_TOKENS is supported by the client
           (see spice_server_migrate_connect), so agent_attached
           is set to FALSE when the agent is disconnected and
           there is no need to track the client tokens
           (see reds_reset_vdp) */
        spice_assert(!agent_dev->priv->agent_attached);
        red_char_device_migrate_data_marshall_empty(m);
        null_agent_mig_data = spice_marshaller_reserve_space(m,
                                                             sizeof(SpiceMigrateDataMain) -
                                                             sizeof(SpiceMigrateDataCharDevice));
        memset(null_agent_mig_data,
               0,
               sizeof(SpiceMigrateDataMain) - sizeof(SpiceMigrateDataCharDevice));
        return;
    }

    red_char_device_migrate_data_marshall(RED_CHAR_DEVICE(agent_dev), m);
    spice_marshaller_add_uint8(m, agent_dev->priv->client_agent_started);

    mig_data.agent2client.chunk_header = agent_dev->priv->vdi_chunk_header;

    /* agent to client partial msg */
    if (agent_dev->priv->read_state == VDI_PORT_READ_STATE_READ_HEADER) {
        mig_data.agent2client.chunk_header_size = agent_dev->priv->receive_pos -
            (uint8_t *)&agent_dev->priv->vdi_chunk_header;

        mig_data.agent2client.msg_header_done = FALSE;
        mig_data.agent2client.msg_header_partial_len = 0;
        spice_assert(!agent_dev->priv->read_filter.msg_data_to_read);
    } else {
        mig_data.agent2client.chunk_header_size = sizeof(VDIChunkHeader);
        mig_data.agent2client.chunk_header.size = agent_dev->priv->message_receive_len;
        if (agent_dev->priv->read_state == VDI_PORT_READ_STATE_READ_DATA) {
            /* in the middle of reading the message header (see reds_on_main_channel_migrate) */
            mig_data.agent2client.msg_header_done = FALSE;
            mig_data.agent2client.msg_header_partial_len =
                agent_dev->priv->receive_pos - agent_dev->priv->current_read_buf->data;
            spice_assert(mig_data.agent2client.msg_header_partial_len < sizeof(VDAgentMessage));
            spice_assert(!agent_dev->priv->read_filter.msg_data_to_read);
        } else {
            mig_data.agent2client.msg_header_done =  TRUE;
            mig_data.agent2client.msg_remaining = agent_dev->priv->read_filter.msg_data_to_read;
            mig_data.agent2client.msg_filter_result = agent_dev->priv->read_filter.result;
        }
    }
    spice_marshaller_add_uint32(m, mig_data.agent2client.chunk_header_size);
    spice_marshaller_add(m,
                         (uint8_t *)&mig_data.agent2client.chunk_header,
                         sizeof(VDIChunkHeader));
    spice_marshaller_add_uint8(m, mig_data.agent2client.msg_header_done);
    spice_marshaller_add_uint32(m, mig_data.agent2client.msg_header_partial_len);
    m2 = spice_marshaller_get_ptr_submarshaller(m, 0);
    spice_marshaller_add(m2, agent_dev->priv->current_read_buf->data,
                         mig_data.agent2client.msg_header_partial_len);
    spice_marshaller_add_uint32(m, mig_data.agent2client.msg_remaining);
    spice_marshaller_add_uint8(m, mig_data.agent2client.msg_filter_result);

    mig_data.client2agent.msg_remaining = agent_dev->priv->write_filter.msg_data_to_read;
    mig_data.client2agent.msg_filter_result = agent_dev->priv->write_filter.result;
    spice_marshaller_add_uint32(m, mig_data.client2agent.msg_remaining);
    spice_marshaller_add_uint8(m, mig_data.client2agent.msg_filter_result);
    spice_debug("from agent filter: discard all %d, wait_msg %u, msg_filter_result %d",
                agent_dev->priv->read_filter.discard_all,
                agent_dev->priv->read_filter.msg_data_to_read,
                agent_dev->priv->read_filter.result);
    spice_debug("to agent filter: discard all %d, wait_msg %u, msg_filter_result %d",
                agent_dev->priv->write_filter.discard_all,
                agent_dev->priv->write_filter.msg_data_to_read,
                agent_dev->priv->write_filter.result);
}

static int reds_agent_state_restore(RedsState *reds, SpiceMigrateDataMain *mig_data)
{
    RedCharDeviceVDIPort *agent_dev = reds->agent_dev;
    uint32_t chunk_header_remaining;

    agent_dev->priv->vdi_chunk_header = mig_data->agent2client.chunk_header;
    spice_assert(mig_data->agent2client.chunk_header_size <= sizeof(VDIChunkHeader));
    chunk_header_remaining = sizeof(VDIChunkHeader) - mig_data->agent2client.chunk_header_size;
    if (chunk_header_remaining) {
        agent_dev->priv->read_state = VDI_PORT_READ_STATE_READ_HEADER;
        agent_dev->priv->receive_pos = (uint8_t *)&agent_dev->priv->vdi_chunk_header +
            mig_data->agent2client.chunk_header_size;
        agent_dev->priv->receive_len = chunk_header_remaining;
    } else {
        agent_dev->priv->message_receive_len = agent_dev->priv->vdi_chunk_header.size;
    }

    if (!mig_data->agent2client.msg_header_done) {
        uint8_t *partial_msg_header;

        if (!chunk_header_remaining) {
            uint32_t cur_buf_size;

            agent_dev->priv->read_state = VDI_PORT_READ_STATE_READ_DATA;
            agent_dev->priv->current_read_buf = vdi_port_get_read_buf(agent_dev);
            spice_assert(agent_dev->priv->current_read_buf);
            partial_msg_header = (uint8_t *)mig_data + mig_data->agent2client.msg_header_ptr -
                sizeof(SpiceMiniDataHeader);
            memcpy(agent_dev->priv->current_read_buf->data,
                   partial_msg_header,
                   mig_data->agent2client.msg_header_partial_len);
            agent_dev->priv->receive_pos = agent_dev->priv->current_read_buf->data +
                                      mig_data->agent2client.msg_header_partial_len;
            cur_buf_size = sizeof(agent_dev->priv->current_read_buf->data) -
                           mig_data->agent2client.msg_header_partial_len;
            agent_dev->priv->receive_len = MIN(agent_dev->priv->message_receive_len, cur_buf_size);
            agent_dev->priv->current_read_buf->len = agent_dev->priv->receive_len +
                                                 mig_data->agent2client.msg_header_partial_len;
            agent_dev->priv->message_receive_len -= agent_dev->priv->receive_len;
        } else {
            spice_assert(mig_data->agent2client.msg_header_partial_len == 0);
        }
    } else {
            agent_dev->priv->read_state = VDI_PORT_READ_STATE_GET_BUFF;
            agent_dev->priv->current_read_buf = NULL;
            agent_dev->priv->receive_pos = NULL;
            agent_dev->priv->read_filter.msg_data_to_read = mig_data->agent2client.msg_remaining;
            agent_dev->priv->read_filter.result = mig_data->agent2client.msg_filter_result;
    }

    agent_dev->priv->read_filter.discard_all = FALSE;
    agent_dev->priv->write_filter.discard_all = !mig_data->client_agent_started;
    agent_dev->priv->client_agent_started = mig_data->client_agent_started;

    agent_dev->priv->write_filter.msg_data_to_read = mig_data->client2agent.msg_remaining;
    agent_dev->priv->write_filter.result = mig_data->client2agent.msg_filter_result;

    spice_debug("to agent filter: discard all %d, wait_msg %u, msg_filter_result %d",
                agent_dev->priv->write_filter.discard_all,
                agent_dev->priv->write_filter.msg_data_to_read,
                agent_dev->priv->write_filter.result);
    spice_debug("from agent filter: discard all %d, wait_msg %u, msg_filter_result %d",
                agent_dev->priv->read_filter.discard_all,
                agent_dev->priv->read_filter.msg_data_to_read,
                agent_dev->priv->read_filter.result);
    return red_char_device_restore(RED_CHAR_DEVICE(agent_dev), &mig_data->agent_base);
}

/*
 * The agent device is not attached to the dest before migration is completed. It is
 * attached only after the vm is started. It might be attached before or after
 * the migration data has reached the server.
 */
bool reds_handle_migrate_data(RedsState *reds, MainChannelClient *mcc,
                              SpiceMigrateDataMain *mig_data, uint32_t size)
{
    RedCharDeviceVDIPort *agent_dev = reds->agent_dev;

    spice_debug("main-channel: got migrate data");
    /*
     * Now that the client has switched to the target server, if main_channel
     * controls the mm-time, we update the client's mm-time.
     * (MSG_MAIN_INIT is not sent for a migrating connection)
     */
    if (reds->mm_time_enabled) {
        reds_send_mm_time(reds);
    }
    if (mig_data->agent_base.connected) {
        if (agent_dev->priv->agent_attached) { // agent was attached before migration data has arrived
            if (!reds->vdagent) {
                spice_assert(agent_dev->priv->plug_generation > 0);
                main_channel_push_agent_disconnected(reds->main_channel);
                spice_debug("agent is no longer connected");
            } else {
                if (agent_dev->priv->plug_generation > 1) {
                    /* red_char_device_state_reset takes care of not making the device wait for migration data */
                    spice_debug("agent has been detached and reattached before receiving migration data");
                    main_channel_push_agent_disconnected(reds->main_channel);
                    main_channel_push_agent_connected(reds->main_channel);
                } else {
                    spice_debug("restoring state from mig_data");
                    return reds_agent_state_restore(reds, mig_data);
                }
            }
        } else {
            /* restore agent state when the agent gets attached */
            spice_debug("saving mig_data");
            spice_assert(agent_dev->priv->plug_generation == 0);
            agent_dev->priv->mig_data = g_memdup(mig_data, size);
        }
    } else {
        spice_debug("agent was not attached on the source host");
        if (reds->vdagent) {
            RedClient *client = red_channel_client_get_client(RED_CHANNEL_CLIENT(mcc));
            /* red_char_device_client_remove disables waiting for migration data */
            red_char_device_client_remove(RED_CHAR_DEVICE(agent_dev), client);
            main_channel_push_agent_connected(reds->main_channel);
        }
    }

    return TRUE;
}

static void reds_channel_init_auth_caps(RedLinkInfo *link, RedChannel *channel)
{
    RedsState *reds = link->reds;
    if (reds->config->sasl_enabled && !link->skip_auth) {
        red_channel_set_common_cap(channel, SPICE_COMMON_CAP_AUTH_SASL);
    } else {
        red_channel_set_common_cap(channel, SPICE_COMMON_CAP_AUTH_SPICE);
    }
    red_channel_set_common_cap(channel, SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
}


static const uint32_t *red_link_info_get_caps(const RedLinkInfo *link)
{
    const uint8_t *caps_start = (const uint8_t *)link->link_mess;

    return (const uint32_t *)(caps_start + link->link_mess->caps_offset);
}

static bool red_link_info_test_capability(const RedLinkInfo *link, uint32_t cap)
{
    const uint32_t *caps = red_link_info_get_caps(link);

    return test_capability(caps, link->link_mess->num_common_caps, cap);
}


static bool reds_send_link_ack(RedsState *reds, RedLinkInfo *link)
{
    struct {
        SpiceLinkHeader header;
        SpiceLinkReply ack;
    } msg;
    RedChannel *channel;
    const RedChannelCapabilities *channel_caps;
    BUF_MEM *bmBuf;
    BIO *bio = NULL;
    int ret = FALSE;
    size_t hdr_size;

    SPICE_VERIFY(sizeof(msg) == sizeof(SpiceLinkHeader) + sizeof(SpiceLinkReply));

    msg.header.magic = SPICE_MAGIC;
    hdr_size = sizeof(msg.ack);
    msg.header.major_version = GUINT32_TO_LE(SPICE_VERSION_MAJOR);
    msg.header.minor_version = GUINT32_TO_LE(SPICE_VERSION_MINOR);

    msg.ack.error = GUINT32_TO_LE(SPICE_LINK_ERR_OK);

    channel = reds_find_channel(reds, link->link_mess->channel_type,
                                link->link_mess->channel_id);
    if (!channel) {
        if (link->link_mess->channel_type != SPICE_CHANNEL_MAIN) {
            spice_warning("Received wrong header: channel_type != SPICE_CHANNEL_MAIN");
            return FALSE;
        }
        spice_assert(reds->main_channel);
        channel = RED_CHANNEL(reds->main_channel);
    }

    reds_channel_init_auth_caps(link, channel); /* make sure common caps are set */

    channel_caps = red_channel_get_local_capabilities(channel);
    msg.ack.num_common_caps = GUINT32_TO_LE(channel_caps->num_common_caps);
    msg.ack.num_channel_caps = GUINT32_TO_LE(channel_caps->num_caps);
    hdr_size += channel_caps->num_common_caps * sizeof(uint32_t);
    hdr_size += channel_caps->num_caps * sizeof(uint32_t);
    msg.header.size = GUINT32_TO_LE(hdr_size);
    msg.ack.caps_offset = GUINT32_TO_LE(sizeof(SpiceLinkReply));
    if (!reds->config->sasl_enabled
        || !red_link_info_test_capability(link, SPICE_COMMON_CAP_AUTH_SASL)) {
        if (!(link->tiTicketing.rsa = RSA_new())) {
            spice_warning("RSA new failed");
            red_dump_openssl_errors();
            return FALSE;
        }

        if (!(bio = BIO_new(BIO_s_mem()))) {
            spice_warning("BIO new failed");
            red_dump_openssl_errors();
            return FALSE;
        }

        if (RSA_generate_key_ex(link->tiTicketing.rsa,
                                SPICE_TICKET_KEY_PAIR_LENGTH,
                                link->tiTicketing.bn,
                                NULL) != 1) {
            spice_warning("Failed to generate %d bits RSA key",
                          SPICE_TICKET_KEY_PAIR_LENGTH);
            red_dump_openssl_errors();
            goto end;
        }
        link->tiTicketing.rsa_size = RSA_size(link->tiTicketing.rsa);

        i2d_RSA_PUBKEY_bio(bio, link->tiTicketing.rsa);
        BIO_get_mem_ptr(bio, &bmBuf);
        memcpy(msg.ack.pub_key, bmBuf->data, sizeof(msg.ack.pub_key));
    } else {
        /* if the client sets the AUTH_SASL cap, it indicates that it
         * supports SASL, and will use it if the server supports SASL as
         * well. Moreover, a client setting the AUTH_SASL cap also
         * indicates that it will not try using the RSA-related content
         * in the SpiceLinkReply message, so we don't need to initialize
         * it. Reason to avoid this is to fix auth in fips mode where
         * the generation of a 1024 bit RSA key as we are trying to do
         * will fail.
         */
        spice_warning("not initialising RSA key");
        memset(msg.ack.pub_key, '\0', sizeof(msg.ack.pub_key));
    }

    if (!red_stream_write_all(link->stream, &msg, sizeof(msg)))
        goto end;
    for (unsigned int i = 0; i < channel_caps->num_common_caps; i++) {
        guint32 cap = GUINT32_TO_LE(channel_caps->common_caps[i]);
        if (!red_stream_write_all(link->stream, &cap, sizeof(cap)))
            goto end;
    }
    for (unsigned int i = 0; i < channel_caps->num_caps; i++) {
        guint32 cap = GUINT32_TO_LE(channel_caps->caps[i]);
        if (!red_stream_write_all(link->stream, &cap, sizeof(cap)))
            goto end;
    }

    ret = TRUE;

end:
    if (bio != NULL)
        BIO_free(bio);
    return ret;
}

static bool reds_send_link_error(RedLinkInfo *link, uint32_t error)
{
    struct {
        SpiceLinkHeader header;
        SpiceLinkReply reply;
    } msg;
    SPICE_VERIFY(sizeof(msg) == sizeof(SpiceLinkHeader) + sizeof(SpiceLinkReply));

    msg.header.magic = SPICE_MAGIC;
    msg.header.size = GUINT32_TO_LE(sizeof(msg.reply));
    msg.header.major_version = GUINT32_TO_LE(SPICE_VERSION_MAJOR);
    msg.header.minor_version = GUINT32_TO_LE(SPICE_VERSION_MINOR);
    memset(&msg.reply, 0, sizeof(msg.reply));
    msg.reply.error = GUINT32_TO_LE(error);
    return red_stream_write_all(link->stream, &msg, sizeof(msg));
}

static void reds_info_new_channel(RedLinkInfo *link, int connection_id)
{
    spice_debug("channel %d:%d, connected successfully, over %s link",
                link->link_mess->channel_type,
                link->link_mess->channel_id,
                red_stream_is_ssl(link->stream) ? "Secure" : "Non Secure");
    /* add info + send event */
    red_stream_set_channel(link->stream, connection_id,
                           link->link_mess->channel_type,
                           link->link_mess->channel_id);
    red_stream_push_channel_event(link->stream, SPICE_CHANNEL_EVENT_INITIALIZED);
}

static void reds_send_link_result(RedLinkInfo *link, uint32_t error)
{
    error = GUINT32_TO_LE(error);
    red_stream_write_all(link->stream, &error, sizeof(error));
}

static void reds_mig_target_client_add(RedsState *reds, RedClient *client)
{
    RedsMigTargetClient *mig_client;

    g_return_if_fail(reds);
    spice_debug("trace");
    mig_client = g_new0(RedsMigTargetClient, 1);
    mig_client->client = client;
    mig_client->reds = reds;
    reds->mig_target_clients = g_list_append(reds->mig_target_clients, mig_client);
}

static RedsMigTargetClient* reds_mig_target_client_find(RedsState *reds, RedClient *client)
{
    GList *l;

    for (l = reds->mig_target_clients; l != NULL; l = l->next) {
        RedsMigTargetClient *mig_client = l->data;

        if (mig_client->client == client) {
            return mig_client;
        }
    }
    return NULL;
}

static void reds_mig_target_client_add_pending_link(RedsMigTargetClient *client,
                                                    SpiceLinkMess *link_msg,
                                                    RedStream *stream)
{
    RedsMigPendingLink *mig_link;

    spice_assert(client);
    mig_link = g_new0(RedsMigPendingLink, 1);
    mig_link->link_msg = link_msg;
    mig_link->stream = stream;

    client->pending_links = g_list_append(client->pending_links, mig_link);
}

static void reds_mig_target_client_free(RedsState *reds, RedsMigTargetClient *mig_client)
{
    reds->mig_target_clients = g_list_remove(reds->mig_target_clients, mig_client);
    g_list_free_full(mig_client->pending_links, g_free);
    g_free(mig_client);
}

static void reds_mig_target_client_disconnect_all(RedsState *reds)
{
    RedsMigTargetClient *mig_client;

    GLIST_FOREACH(reds->mig_target_clients, RedsMigTargetClient, mig_client) {
        reds_client_disconnect(reds, mig_client->client);
    }
}

static bool reds_find_client(RedsState *reds, RedClient *client)
{
    RedClient *list_client;

    GLIST_FOREACH(reds->clients, RedClient, list_client) {
        if (list_client == client) {
            return TRUE;
        }
    }
    return FALSE;
}

/* should be used only when there is one client */
static RedClient *reds_get_client(RedsState *reds)
{
    gint n = g_list_length(reds->clients);
    spice_assert(n <= 1);

    if (n == 0) {
        return NULL;
    }

    return reds->clients->data;
}

/* Performs late initializations steps.
 * This should be called when a client connects */
static void reds_late_initialization(RedsState *reds)
{
    RedCharDevice *dev;

    // do only once
    if (reds->late_initialization_done) {
        return;
    }

    // create stream channels for streaming devices
    GLIST_FOREACH(reds->char_devices, RedCharDevice, dev) {
        if (IS_STREAM_DEVICE(dev)) {
            stream_device_create_channel(STREAM_DEVICE(dev));
        }
    }
    reds->late_initialization_done = true;
}

static void
red_channel_capabilities_init_from_link_message(RedChannelCapabilities *caps,
                                                const SpiceLinkMess *link_mess)
{
    const uint8_t *raw_caps = (const uint8_t *)link_mess + link_mess->caps_offset;

    caps->num_common_caps = link_mess->num_common_caps;
    caps->common_caps = NULL;
    if (caps->num_common_caps) {
        caps->common_caps = g_memdup(raw_caps,
                                     link_mess->num_common_caps * sizeof(uint32_t));
    }
    caps->num_caps = link_mess->num_channel_caps;
    caps->caps = NULL;
    if (link_mess->num_channel_caps) {
        caps->caps = g_memdup(raw_caps + link_mess->num_common_caps * sizeof(uint32_t),
                              link_mess->num_channel_caps * sizeof(uint32_t));
    }
}

// TODO: now that main is a separate channel this should
// actually be joined with reds_handle_other_links, become reds_handle_link
static void reds_handle_main_link(RedsState *reds, RedLinkInfo *link)
{
    RedClient *client;
    RedStream *stream;
    SpiceLinkMess *link_mess;
    uint32_t connection_id;
    MainChannelClient *mcc;
    int mig_target = FALSE;
    RedChannelCapabilities caps;

    spice_debug("trace");
    spice_assert(reds->main_channel);

    reds_late_initialization(reds);

    link_mess = link->link_mess;
    if (!reds->allow_multiple_clients) {
        reds_disconnect(reds);
    }

    if (link_mess->connection_id == 0) {
        reds_send_link_result(link, SPICE_LINK_ERR_OK);
        while((connection_id = rand()) == 0);
        mig_target = FALSE;
    } else {
        // TODO: make sure link_mess->connection_id is the same
        // connection id the migration src had (use vmstate to store the connection id)
        reds_send_link_result(link, SPICE_LINK_ERR_OK);
        connection_id = link_mess->connection_id;
        mig_target = TRUE;
    }

    reds->mig_inprogress = FALSE;
    reds->mig_wait_connect = FALSE;
    reds->mig_wait_disconnect = FALSE;

    reds_info_new_channel(link, connection_id);
    stream = link->stream;
    link->stream = NULL;
    link->link_mess = NULL;
    reds_link_free(link);
    client = red_client_new(reds, mig_target);
    reds->clients = g_list_prepend(reds->clients, client);

    red_channel_capabilities_init_from_link_message(&caps, link_mess);
    mcc = main_channel_link(reds->main_channel, client,
                            stream, connection_id, mig_target,
                            &caps);
    red_channel_capabilities_reset(&caps);
    spice_debug("NEW Client %p mcc %p connect-id %d", client, mcc, connection_id);
    g_free(link_mess);
    red_client_set_main(client, mcc);

    if (reds->vdagent) {
        if (mig_target) {
            spice_warning("unexpected: vdagent attached to destination during migration");
        }
        agent_msg_filter_config(&reds->agent_dev->priv->read_filter,
                                reds->config->agent_copypaste,
                                reds->config->agent_file_xfer,
                                reds_use_client_monitors_config(reds));
        reds->agent_dev->priv->read_filter.discard_all = FALSE;
        reds->agent_dev->priv->plug_generation++;
    }

    if (!mig_target) {
        main_channel_client_push_init(mcc, g_list_length(reds->qxl_instances),
            reds->mouse_mode, reds->is_client_mouse_allowed,
            reds_get_mm_time() - MM_TIME_DELTA,
            reds_qxl_ram_size(reds));
        if (reds->config->spice_name)
            main_channel_client_push_name(mcc, reds->config->spice_name);
        if (reds->config->spice_uuid_is_set)
            main_channel_client_push_uuid(mcc, reds->config->spice_uuid);
    } else {
        reds_mig_target_client_add(reds, client);
    }

    if (red_stream_get_family(stream) != AF_UNIX)
        main_channel_client_start_net_test(mcc, !mig_target);
}

#define RED_MOUSE_STATE_TO_LOCAL(state)     \
    ((state & SPICE_MOUSE_BUTTON_MASK_LEFT) |          \
     ((state & SPICE_MOUSE_BUTTON_MASK_MIDDLE) << 1) |   \
     ((state & SPICE_MOUSE_BUTTON_MASK_RIGHT) >> 1))

#define RED_MOUSE_BUTTON_STATE_TO_AGENT(state)                      \
    (((state & SPICE_MOUSE_BUTTON_MASK_LEFT) ? VD_AGENT_LBUTTON_MASK : 0) |    \
     ((state & SPICE_MOUSE_BUTTON_MASK_MIDDLE) ? VD_AGENT_MBUTTON_MASK : 0) |    \
     ((state & SPICE_MOUSE_BUTTON_MASK_RIGHT) ? VD_AGENT_RBUTTON_MASK : 0))

static void openssl_init(RedLinkInfo *link)
{
    unsigned long f4 = RSA_F4;
    link->tiTicketing.bn = BN_new();

    if (!link->tiTicketing.bn) {
        red_dump_openssl_errors();
        spice_error("OpenSSL BIGNUMS alloc failed");
    }

    BN_set_word(link->tiTicketing.bn, f4);
}

static void reds_channel_do_link(RedChannel *channel, RedClient *client,
                                 SpiceLinkMess *link_msg,
                                 RedStream *stream)
{
    RedChannelCapabilities caps;

    spice_assert(channel);
    spice_assert(link_msg);
    spice_assert(stream);

    red_channel_capabilities_init_from_link_message(&caps, link_msg);
    red_channel_connect(channel, client, stream,
                        red_client_during_migrate_at_target(client),
                        &caps);
    red_channel_capabilities_reset(&caps);
}

/*
 * migration target side:
 * In semi-seamless migration, we activate the channels only
 * after migration is completed.
 * In seamless migration, in order to keep the continuousness, and
 * not lose any data, we activate the target channels before
 * migration completes, as soon as we receive SPICE_MSGC_MAIN_MIGRATE_DST_DO_SEAMLESS
 */
static bool reds_link_mig_target_channels(RedsState *reds, RedClient *client)
{
    RedsMigTargetClient *mig_client;
    GList *item;

    spice_debug("%p", client);
    mig_client = reds_mig_target_client_find(reds, client);
    if (!mig_client) {
        spice_debug("Error: mig target client was not found");
        return FALSE;
    }

    /* Each channel should check if we are during migration, and
     * act accordingly. */
    for(item = mig_client->pending_links; item != NULL; item = item->next) {
        RedsMigPendingLink *mig_link = item->data;
        RedChannel *channel;

        channel = reds_find_channel(reds, mig_link->link_msg->channel_type,
                                    mig_link->link_msg->channel_id);
        if (!channel) {
            spice_warning("client %p channel (%d, %d) (type, id) wasn't found",
                          client,
                          mig_link->link_msg->channel_type,
                          mig_link->link_msg->channel_id);
            continue;
        }
        reds_channel_do_link(channel, client, mig_link->link_msg, mig_link->stream);
    }

    reds_mig_target_client_free(reds, mig_client);

    return TRUE;
}

int reds_on_migrate_dst_set_seamless(RedsState *reds, MainChannelClient *mcc, uint32_t src_version)
{
    /* seamless migration is not supported with multiple clients*/
    if (reds->allow_multiple_clients  || src_version > SPICE_MIGRATION_PROTOCOL_VERSION) {
        reds->dst_do_seamless_migrate = FALSE;
    } else {
        RedChannelClient *rcc = RED_CHANNEL_CLIENT(mcc);
        RedClient *client = red_channel_client_get_client(rcc);

        red_client_set_migration_seamless(client);
        /* linking all the channels that have been connected before migration handshake */
        reds->dst_do_seamless_migrate = reds_link_mig_target_channels(reds, client);
    }
    return reds->dst_do_seamless_migrate;
}

void reds_on_client_seamless_migrate_complete(RedsState *reds, RedClient *client)
{
    spice_debug("trace");
    if (!reds_find_client(reds, client)) {
        spice_debug("client no longer exists");
        return;
    }
    main_channel_client_migrate_dst_complete(red_client_get_main(client));
}

void reds_on_client_semi_seamless_migrate_complete(RedsState *reds, RedClient *client)
{
    MainChannelClient *mcc;

    spice_debug("%p", client);
    mcc = red_client_get_main(client);

    // TODO: not doing net test. consider doing it on client_migrate_info
    main_channel_client_push_init(mcc, g_list_length(reds->qxl_instances),
                                  reds->mouse_mode,
                                  reds->is_client_mouse_allowed,
                                  reds_get_mm_time() - MM_TIME_DELTA,
                                  reds_qxl_ram_size(reds));
    reds_link_mig_target_channels(reds, client);
    main_channel_client_migrate_dst_complete(mcc);
}

static void reds_handle_other_links(RedsState *reds, RedLinkInfo *link)
{
    RedChannel *channel;
    RedClient *client = NULL;
    SpiceLinkMess *link_mess;
    RedsMigTargetClient *mig_client;

    link_mess = link->link_mess;
    if (reds->main_channel) {
        client = main_channel_get_client_by_link_id(reds->main_channel,
                                                    link_mess->connection_id);
    }

    // TODO: MC: broke migration (at least for the dont-drop-connection kind).
    // On migration we should get a connection_id to expect (must be a security measure)
    // where do we store it? on reds, but should be a list (MC).
    if (!client) {
        reds_send_link_result(link, SPICE_LINK_ERR_BAD_CONNECTION_ID);
        reds_link_free(link);
        return;
    }

    // TODO: MC: be less lenient. Tally connections from same connection_id (by same client).
    if (!(channel = reds_find_channel(reds, link_mess->channel_type,
                                      link_mess->channel_id))) {
        reds_send_link_result(link, SPICE_LINK_ERR_CHANNEL_NOT_AVAILABLE);
        reds_link_free(link);
        return;
    }

    reds_send_link_result(link, SPICE_LINK_ERR_OK);
    reds_info_new_channel(link, link_mess->connection_id);

    mig_client = reds_mig_target_client_find(reds, client);
    /*
     * In semi-seamless migration, we activate the channels only
     * after migration is completed. Since, the session starts almost from
     * scratch we don't mind if we skip some messages in between the src session end and
     * dst session start.
     * In seamless migration, in order to keep the continuousness of the session, and
     * in order not to lose any data, we activate the target channels before
     * migration completes, as soon as we receive SPICE_MSGC_MAIN_MIGRATE_DST_DO_SEAMLESS.
     * If a channel connects before receiving SPICE_MSGC_MAIN_MIGRATE_DST_DO_SEAMLESS,
     * reds_on_migrate_dst_set_seamless will take care of activating it */
    if (red_client_during_migrate_at_target(client) && !reds->dst_do_seamless_migrate) {
        spice_assert(mig_client);
        reds_mig_target_client_add_pending_link(mig_client, link_mess, link->stream);
    } else {
        spice_assert(!mig_client);
        reds_channel_do_link(channel, client, link_mess, link->stream);
        g_free(link_mess);
    }
    link->stream = NULL;
    link->link_mess = NULL;
    reds_link_free(link);
}

static void reds_handle_link(RedLinkInfo *link)
{
    RedsState *reds = link->reds;

    red_stream_remove_watch(link->stream);
    if (link->link_mess->channel_type == SPICE_CHANNEL_MAIN) {
        reds_handle_main_link(reds, link);
    } else {
        reds_handle_other_links(reds, link);
    }
}

static void reds_handle_ticket(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    RedsState *reds = link->reds;
    char *password;
    int password_size;

    if (RSA_size(link->tiTicketing.rsa) < SPICE_MAX_PASSWORD_LENGTH) {
        spice_warning("RSA modulus size is smaller than SPICE_MAX_PASSWORD_LENGTH (%d < %d), "
                      "SPICE ticket sent from client may be truncated",
                      RSA_size(link->tiTicketing.rsa), SPICE_MAX_PASSWORD_LENGTH);
    }

    password = g_new0(char, RSA_size(link->tiTicketing.rsa) + 1);
    password_size = RSA_private_decrypt(link->tiTicketing.rsa_size,
                                        link->tiTicketing.encrypted_ticket.encrypted_data,
                                        (unsigned char *)password,
                                        link->tiTicketing.rsa,
                                        RSA_PKCS1_OAEP_PADDING);
    if (password_size == -1) {
        spice_warning("failed to decrypt RSA encrypted password");
        red_dump_openssl_errors();
        goto error;
    }
    password[password_size] = '\0';

    if (reds->config->ticketing_enabled && !link->skip_auth) {
        time_t ltime;
        bool expired;

        if (strlen(reds->config->taTicket.password) == 0) {
            spice_warning("Ticketing is enabled, but no password is set. "
                          "please set a ticket first");
            goto error;
        }

        ltime = spice_get_monotonic_time_ns() / NSEC_PER_SEC;
        expired = (reds->config->taTicket.expiration_time < ltime);

        if (expired) {
            spice_warning("Ticket has expired");
            goto error;
        }

        if (strcmp(password, reds->config->taTicket.password) != 0) {
            spice_warning("Invalid password");
            goto error;
        }
    }

    reds_handle_link(link);
    goto end;

error:
    reds_send_link_result(link, SPICE_LINK_ERR_PERMISSION_DENIED);
    reds_link_free(link);

end:
    g_free(password);
}

static void reds_get_spice_ticket(RedLinkInfo *link)
{
    red_stream_async_read(link->stream,
                          (uint8_t *)&link->tiTicketing.encrypted_ticket.encrypted_data,
                          link->tiTicketing.rsa_size, reds_handle_ticket, link);
}

#if HAVE_SASL
static void reds_handle_sasl_result(void *opaque, RedSaslError status)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;

    switch (status) {
    case RED_SASL_ERROR_OK:
        reds_handle_link(link);
        break;
    case RED_SASL_ERROR_INVALID_DATA:
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
        reds_link_free(link);
        break;
    default:
        // in these cases error was reported using SASL protocol
        // (RED_SASL_ERROR_AUTH_FAILED) or we just need to close the
        // connection
        reds_link_free(link);
        break;
    }
}

static void reds_start_auth_sasl(RedLinkInfo *link)
{
    if (!red_sasl_start_auth(link->stream, reds_handle_sasl_result, link)) {
        reds_link_free(link);
    }
}
#endif

static void reds_handle_auth_mechanism(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    RedsState *reds = link->reds;

    spice_debug("Auth method: %d", link->auth_mechanism.auth_mechanism);

    link->auth_mechanism.auth_mechanism = GUINT32_FROM_LE(link->auth_mechanism.auth_mechanism);
    if (link->auth_mechanism.auth_mechanism == SPICE_COMMON_CAP_AUTH_SPICE
        && !reds->config->sasl_enabled
        ) {
        reds_get_spice_ticket(link);
#if HAVE_SASL
    } else if (link->auth_mechanism.auth_mechanism == SPICE_COMMON_CAP_AUTH_SASL) {
        spice_debug("Starting SASL");
        reds_start_auth_sasl(link);
#endif
    } else {
        spice_warning("Unknown auth method, disconnecting");
        if (reds->config->sasl_enabled) {
            spice_warning("Your client doesn't handle SASL?");
        }
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
        reds_link_free(link);
    }
}

static int reds_security_check(RedLinkInfo *link)
{
    RedsState *reds = link->reds;
    ChannelSecurityOptions *security_option = reds_find_channel_security(reds, link->link_mess->channel_type);
    uint32_t security = security_option ? security_option->options : reds->config->default_channel_security;
    return (red_stream_is_ssl(link->stream) && (security & SPICE_CHANNEL_SECURITY_SSL)) ||
        (!red_stream_is_ssl(link->stream) && (security & SPICE_CHANNEL_SECURITY_NONE));
}

static void reds_handle_read_link_done(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    RedsState *reds = link->reds;
    SpiceLinkMess *link_mess = link->link_mess;
    uint32_t num_caps;
    uint32_t *caps;
    int auth_selection;
    unsigned int i;

    link_mess->caps_offset = GUINT32_FROM_LE(link_mess->caps_offset);
    link_mess->connection_id = GUINT32_FROM_LE(link_mess->connection_id);
    link_mess->num_channel_caps = GUINT32_FROM_LE(link_mess->num_channel_caps);
    link_mess->num_common_caps = GUINT32_FROM_LE(link_mess->num_common_caps);

    /* Prevent DoS. Currently we defined only 13 capabilities,
     * I expect 1024 to be valid for quite a lot time */
    if (link_mess->num_channel_caps > 1024 || link_mess->num_common_caps > 1024) {
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
        reds_link_free(link);
        return;
    }

    num_caps = link_mess->num_common_caps + link_mess->num_channel_caps;
    caps = (uint32_t *)((uint8_t *)link_mess + link_mess->caps_offset);

    if (num_caps && (num_caps * sizeof(uint32_t) + link_mess->caps_offset >
                     link->link_header.size ||
                     link_mess->caps_offset < sizeof(*link_mess))) {
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
        reds_link_free(link);
        return;
    }

    for(i = 0; i < num_caps;i++)
        caps[i] = GUINT32_FROM_LE(caps[i]);

    auth_selection = red_link_info_test_capability(link,
                                                   SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);

    if (!reds_security_check(link)) {
        if (red_stream_is_ssl(link->stream)) {
            spice_warning("spice channels %d should not be encrypted", link_mess->channel_type);
            reds_send_link_error(link, SPICE_LINK_ERR_NEED_UNSECURED);
        } else {
            spice_warning("spice channels %d should be encrypted", link_mess->channel_type);
            reds_send_link_error(link, SPICE_LINK_ERR_NEED_SECURED);
        }
        reds_link_free(link);
        return;
    }

    if (!reds_send_link_ack(reds, link)) {
        reds_link_free(link);
        return;
    }

    if (!auth_selection) {
        if (reds->config->sasl_enabled && !link->skip_auth) {
            spice_warning("SASL enabled, but peer supports only spice authentication");
            reds_send_link_error(link, SPICE_LINK_ERR_VERSION_MISMATCH);
            return;
        }
        spice_warning("Peer doesn't support AUTH selection");
        reds_get_spice_ticket(link);
    } else {
        red_stream_async_read(link->stream,
                              (uint8_t *)&link->auth_mechanism,
                              sizeof(SpiceLinkAuthMechanism),
                              reds_handle_auth_mechanism,
                              link);
    }
}

static void reds_handle_link_error(void *opaque, int err)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    switch (err) {
    case 0:
    case EPIPE:
        break;
    default:
        spice_warning("%s", strerror(errno));
        break;
    }
    reds_link_free(link);
}

static void reds_handle_read_header_done(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    SpiceLinkHeader *header = &link->link_header;

    header->major_version = GUINT32_FROM_LE(header->major_version);
    header->minor_version = GUINT32_FROM_LE(header->minor_version);
    header->size = GUINT32_FROM_LE(header->size);

    if (header->major_version != SPICE_VERSION_MAJOR) {
        if (header->major_version > 0) {
            reds_send_link_error(link, SPICE_LINK_ERR_VERSION_MISMATCH);
        }

        spice_warning("version mismatch");
        reds_link_free(link);
        return;
    }

    /* the check for 4096 is to avoid clients to cause arbitrary big memory allocations */
    if (header->size < sizeof(SpiceLinkMess) || header->size > 4096) {
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_DATA);
        spice_warning("bad size %u", header->size);
        reds_link_free(link);
        return;
    }

    link->link_mess = g_malloc(header->size);

    red_stream_async_read(link->stream,
                          (uint8_t *)link->link_mess,
                          header->size,
                          reds_handle_read_link_done,
                          link);
}

static void reds_handle_read_magic_done(void *opaque)
{
    RedLinkInfo *link = (RedLinkInfo *)opaque;
    const SpiceLinkHeader *header = &link->link_header;

    if (header->magic != SPICE_MAGIC) {
        reds_send_link_error(link, SPICE_LINK_ERR_INVALID_MAGIC);
        reds_link_free(link);
        return;
    }

    red_stream_async_read(link->stream,
                          ((uint8_t *)&link->link_header) + sizeof(header->magic),
                          sizeof(SpiceLinkHeader) - sizeof(header->magic),
                          reds_handle_read_header_done,
                          link);
}

static void reds_handle_new_link(RedLinkInfo *link)
{
    red_stream_set_async_error_handler(link->stream, reds_handle_link_error);
    red_stream_async_read(link->stream,
                          (uint8_t *)&link->link_header,
                          sizeof(link->link_header.magic),
                          reds_handle_read_magic_done,
                          link);
}

static void reds_handle_ssl_accept(int fd, int event, void *data)
{
    RedLinkInfo *link = (RedLinkInfo *)data;
    RedsState *reds = link->reds;
    RedStreamSslStatus return_code = red_stream_ssl_accept(link->stream);

    switch (return_code) {
        case RED_STREAM_SSL_STATUS_ERROR:
            reds_link_free(link);
            return;
        case RED_STREAM_SSL_STATUS_WAIT_FOR_READ:
            reds_core_watch_update_mask(reds, link->stream->watch,
                                        SPICE_WATCH_EVENT_READ);
            return;
        case RED_STREAM_SSL_STATUS_WAIT_FOR_WRITE:
            reds_core_watch_update_mask(reds, link->stream->watch,
                                        SPICE_WATCH_EVENT_WRITE);
            return;
        case RED_STREAM_SSL_STATUS_OK:
            red_stream_remove_watch(link->stream);
            reds_handle_new_link(link);
    }
}

#define KEEPALIVE_TIMEOUT (10*60)

static RedLinkInfo *reds_init_client_connection(RedsState *reds, int socket)
{
    RedLinkInfo *link;

    if (!red_socket_set_non_blocking(socket, TRUE)) {
       goto error;
    }

    if (!red_socket_set_no_delay(socket, TRUE)) {
       goto error;
    }

    red_socket_set_keepalive(socket, TRUE, KEEPALIVE_TIMEOUT);

    link = g_new0(RedLinkInfo, 1);
    link->reds = reds;
    link->stream = red_stream_new(reds, socket);

    /* gather info + send event */

    red_stream_push_channel_event(link->stream, SPICE_CHANNEL_EVENT_CONNECTED);

    openssl_init(link);

    return link;

error:
    return NULL;
}


static RedLinkInfo *reds_init_client_ssl_connection(RedsState *reds, int socket)
{
    RedLinkInfo *link;
    RedStreamSslStatus ssl_status;

    link = reds_init_client_connection(reds, socket);
    if (link == NULL) {
        return NULL;
    }

    ssl_status = red_stream_enable_ssl(link->stream, reds->ctx);
    switch (ssl_status) {
        case RED_STREAM_SSL_STATUS_OK:
            reds_handle_new_link(link);
            return link;
        case RED_STREAM_SSL_STATUS_ERROR:
            goto error;
        case RED_STREAM_SSL_STATUS_WAIT_FOR_READ:
            link->stream->watch = reds_core_watch_add(reds, link->stream->socket,
                                                      SPICE_WATCH_EVENT_READ,
                                                      reds_handle_ssl_accept, link);
            break;
        case RED_STREAM_SSL_STATUS_WAIT_FOR_WRITE:
            link->stream->watch = reds_core_watch_add(reds, link->stream->socket,
                                                      SPICE_WATCH_EVENT_WRITE,
                                                      reds_handle_ssl_accept, link);
            break;
    }
    return link;

error:
    /* close the stream but do not close the socket, this API is
     * supposed to not close it if it fails */
    link->stream->socket = -1;
    reds_link_free(link);
    return NULL;
}

static void reds_accept_ssl_connection(int fd, int event, void *data)
{
    RedsState *reds = data;
    RedLinkInfo *link;
    int socket;

    if ((socket = accept(fd, NULL, 0)) == -1) {
        spice_warning("accept failed, %s", strerror(errno));
        return;
    }

    if (!(link = reds_init_client_ssl_connection(reds, socket))) {
        close(socket);
        return;
    }
}


static void reds_accept(int fd, int event, void *data)
{
    RedsState *reds = data;
    int socket;

    if ((socket = accept(fd, NULL, 0)) == -1) {
        spice_warning("accept failed, %s", strerror(errno));
        return;
    }

    if (spice_server_add_client(reds, socket, 0) < 0)
        close(socket);
}


SPICE_GNUC_VISIBLE int spice_server_add_client(SpiceServer *reds, int socket, int skip_auth)
{
    RedLinkInfo *link;

    if (!(link = reds_init_client_connection(reds, socket))) {
        spice_warning("accept failed");
        return -1;
    }

    link->skip_auth = skip_auth;

    reds_handle_new_link(link);
    return 0;
}


SPICE_GNUC_VISIBLE int spice_server_add_ssl_client(SpiceServer *reds, int socket, int skip_auth)
{
    RedLinkInfo *link;

    if (!(link = reds_init_client_ssl_connection(reds, socket))) {
        return -1;
    }

    link->skip_auth = skip_auth;
    return 0;
}


static int reds_init_socket(const char *addr, int portnr, int family)
{
    static const int on=1, off=0;
    struct addrinfo ai,*res,*e;
    char port[33];
    int slisten, rc, len;

    if (family == AF_UNIX) {
        struct sockaddr_un local = { 0, };

        if ((slisten = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
            perror("socket");
            return -1;
        }

        local.sun_family = AF_UNIX;
        g_strlcpy(local.sun_path, addr, sizeof(local.sun_path));
        unlink(local.sun_path);
        len = SUN_LEN(&local);
        if (bind(slisten, (struct sockaddr *)&local, len) == -1) {
            perror("bind");
            close(slisten);
            return -1;
        }
        chmod(addr, 0666);

        goto listen;
    }

    memset(&ai,0, sizeof(ai));
    ai.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
    ai.ai_socktype = SOCK_STREAM;
    ai.ai_family = family;

    snprintf(port, sizeof(port), "%d", portnr);
    rc = getaddrinfo(strlen(addr) ? addr : NULL, port, &ai, &res);
    if (rc != 0) {
        spice_warning("getaddrinfo(%s,%s): %s", addr, port,
                      gai_strerror(rc));
        return -1;
    }

    for (e = res; e != NULL; e = e->ai_next) {
        slisten = socket(e->ai_family, e->ai_socktype, e->ai_protocol);
        if (slisten < 0) {
            continue;
        }

        setsockopt(slisten,SOL_SOCKET,SO_REUSEADDR,(void*)&on,sizeof(on));
#ifdef IPV6_V6ONLY
        if (e->ai_family == PF_INET6) {
            /* listen on both ipv4 and ipv6 */
            setsockopt(slisten,IPPROTO_IPV6,IPV6_V6ONLY,(void*)&off,
                       sizeof(off));
        }
#endif
        if (bind(slisten, e->ai_addr, e->ai_addrlen) == 0) {
            char uaddr[INET6_ADDRSTRLEN+1];
            char uport[33];
            rc = getnameinfo((struct sockaddr*)e->ai_addr,e->ai_addrlen,
                             uaddr,INET6_ADDRSTRLEN, uport,32,
                             NI_NUMERICHOST | NI_NUMERICSERV);
            if (rc == 0) {
                spice_debug("bound to %s:%s", uaddr, uport);
            } else {
                spice_debug("cannot resolve address spice-server is bound to");
            }
            freeaddrinfo(res);
            goto listen;
        }
        close(slisten);
    }
    spice_warning("binding socket to %s:%d failed", addr, portnr);
    freeaddrinfo(res);
    return -1;

listen:
    if (listen(slisten, SOMAXCONN) != 0) {
        spice_warning("listen: %s", strerror(errno));
        close(slisten);
        return -1;
    }
    return slisten;
}

static void reds_send_mm_time(RedsState *reds)
{
    if (!reds_main_channel_connected(reds)) {
        return;
    }
    spice_debug("trace");
    main_channel_push_multi_media_time(reds->main_channel,
                                       reds_get_mm_time() - reds->mm_time_latency);
}

void reds_set_client_mm_time_latency(RedsState *reds, RedClient *client, uint32_t latency)
{
    // TODO: multi-client support for mm_time
    if (reds->mm_time_enabled) {
        // TODO: consider network latency
        if (latency > reds->mm_time_latency) {
            reds->mm_time_latency = latency;
            reds_send_mm_time(reds);
        } else {
            spice_debug("new latency %u is smaller than existing %u",
                        latency, reds->mm_time_latency);
        }
    } else {
        snd_set_playback_latency(client, latency);
    }
}

static void reds_cleanup_net(SpiceServer *reds)
{
    if (reds->listen_socket != -1) {
       reds_core_watch_remove(reds, reds->listen_watch);
       if (reds->config->spice_listen_socket_fd != reds->listen_socket) {
          close(reds->listen_socket);
       }
       reds->listen_watch = NULL;
       reds->listen_socket = -1;
    }
    if (reds->secure_listen_socket != -1) {
       reds_core_watch_remove(reds, reds->secure_listen_watch);
       close(reds->secure_listen_socket);
       reds->secure_listen_watch = NULL;
       reds->secure_listen_socket = -1;
    }
}

static int reds_init_net(RedsState *reds)
{
    if (reds->config->spice_port != -1 || reds->config->spice_family == AF_UNIX) {
        reds->listen_socket = reds_init_socket(reds->config->spice_addr, reds->config->spice_port, reds->config->spice_family);
        if (-1 == reds->listen_socket) {
            return -1;
        }
        reds->listen_watch = reds_core_watch_add(reds, reds->listen_socket,
                                                 SPICE_WATCH_EVENT_READ,
                                                 reds_accept, reds);
        if (reds->listen_watch == NULL) {
            return -1;
        }
    }

    if (reds->config->spice_secure_port != -1) {
        reds->secure_listen_socket = reds_init_socket(reds->config->spice_addr, reds->config->spice_secure_port,
                                                      reds->config->spice_family);
        if (-1 == reds->secure_listen_socket) {
            return -1;
        }
        reds->secure_listen_watch = reds_core_watch_add(reds, reds->secure_listen_socket,
                                                        SPICE_WATCH_EVENT_READ,
                                                        reds_accept_ssl_connection, reds);
        if (reds->secure_listen_watch == NULL) {
            return -1;
        }
    }

    if (reds->config->spice_listen_socket_fd != -1 ) {
        reds->listen_socket = reds->config->spice_listen_socket_fd;
        reds->listen_watch = reds_core_watch_add(reds, reds->listen_socket,
                                                 SPICE_WATCH_EVENT_READ,
                                                 reds_accept, reds);
        if (reds->listen_watch == NULL) {
            return -1;
        }
    }
    return 0;
}

static int load_dh_params(SSL_CTX *ctx, char *file)
{
    DH *ret = 0;
    BIO *bio;

    if ((bio = BIO_new_file(file, "r")) == NULL) {
        spice_warning("Could not open DH file");
        red_dump_openssl_errors();
        return -1;
    }

    ret = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (ret == 0) {
        spice_warning("Could not read DH params");
        red_dump_openssl_errors();
        return -1;
    }


    if (SSL_CTX_set_tmp_dh(ctx, ret) < 0) {
        spice_warning("Could not set DH params");
        red_dump_openssl_errors();
        return -1;
    }

    return 0;
}

/*The password code is not thread safe*/
static int ssl_password_cb(char *buf, int size, int flags, void *userdata)
{
    RedsState *reds = userdata;
    char *pass = reds->config->ssl_parameters.keyfile_password;
    if (size < strlen(pass) + 1) {
        return (0);
    }

    strcpy(buf, pass);
    return (strlen(pass));
}

#if OPENSSL_VERSION_NUMBER < 0x1010000FL
static pthread_mutex_t *lock_cs;

static unsigned long pthreads_thread_id(void)
{
    unsigned long ret;

    ret = (unsigned long)pthread_self();
    return (ret);
}

static void pthreads_locking_callback(int mode, int type, const char *file, int line)
{
    if (mode & CRYPTO_LOCK) {
        pthread_mutex_lock(&(lock_cs[type]));
    } else {
        pthread_mutex_unlock(&(lock_cs[type]));
    }
}

static void openssl_thread_setup(void)
{
    int i;

    /* Somebody else already setup threading for OpenSSL,
     * don't do it twice to avoid possible races.
     */
    if (CRYPTO_get_locking_callback() != NULL) {
        red_dump_openssl_errors();
        return;
    }

    lock_cs = OPENSSL_malloc(CRYPTO_num_locks() * sizeof(pthread_mutex_t));

    for (i = 0; i < CRYPTO_num_locks(); i++) {
        pthread_mutex_init(&(lock_cs[i]), NULL);
    }

    CRYPTO_set_id_callback(pthreads_thread_id);
    CRYPTO_set_locking_callback(pthreads_locking_callback);
}
#else
static inline void openssl_thread_setup(void)
{
}
#endif

static gpointer openssl_global_init(gpointer arg)
{
    SSL_library_init();
    SSL_load_error_strings();

    openssl_thread_setup();

    return NULL;
}

static int reds_init_ssl(RedsState *reds)
{
    static GOnce openssl_once = G_ONCE_INIT;
    const SSL_METHOD *ssl_method;
    int return_code;
    /* Limit connection to TLSv1.1 or newer.
     * When some other SSL/TLS version becomes obsolete, add it to this
     * variable. */
    long ssl_options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION | SSL_OP_NO_TLSv1;

    /* Global system initialization*/
    g_once(&openssl_once, openssl_global_init, NULL);

    /* Create our context*/
    /* SSLv23_method() handles TLSv1.x in addition to SSLv2/v3 */
    ssl_method = SSLv23_method();
    reds->ctx = SSL_CTX_new(ssl_method);
    if (!reds->ctx) {
        spice_warning("Could not allocate new SSL context");
        red_dump_openssl_errors();
        return -1;
    }

    SSL_CTX_set_options(reds->ctx, ssl_options);
    SSL_CTX_set_ecdh_auto(reds->ctx, 1);

    /* Load our keys and certificates*/
    return_code = SSL_CTX_use_certificate_chain_file(reds->ctx, reds->config->ssl_parameters.certs_file);
    if (return_code == 1) {
        spice_debug("Loaded certificates from %s", reds->config->ssl_parameters.certs_file);
    } else {
        spice_warning("Could not load certificates from %s", reds->config->ssl_parameters.certs_file);
        red_dump_openssl_errors();
        return -1;
    }

    SSL_CTX_set_default_passwd_cb(reds->ctx, ssl_password_cb);
    SSL_CTX_set_default_passwd_cb_userdata(reds->ctx, reds);

    return_code = SSL_CTX_use_PrivateKey_file(reds->ctx, reds->config->ssl_parameters.private_key_file,
                                              SSL_FILETYPE_PEM);
    if (return_code == 1) {
        spice_debug("Using private key from %s", reds->config->ssl_parameters.private_key_file);
    } else {
        spice_warning("Could not use private key file");
        return -1;
    }

    /* Load the CAs we trust*/
    return_code = SSL_CTX_load_verify_locations(reds->ctx, reds->config->ssl_parameters.ca_certificate_file, 0);
    if (return_code == 1) {
        spice_debug("Loaded CA certificates from %s", reds->config->ssl_parameters.ca_certificate_file);
    } else {
        spice_warning("Could not use CA file %s", reds->config->ssl_parameters.ca_certificate_file);
        red_dump_openssl_errors();
        return -1;
    }

    if (strlen(reds->config->ssl_parameters.dh_key_file) > 0) {
        if (load_dh_params(reds->ctx, reds->config->ssl_parameters.dh_key_file) < 0) {
            return -1;
        }
    }

    SSL_CTX_set_session_id_context(reds->ctx, (const unsigned char *)"SPICE", 5);
    if (strlen(reds->config->ssl_parameters.ciphersuite) > 0) {
        if (!SSL_CTX_set_cipher_list(reds->ctx, reds->config->ssl_parameters.ciphersuite)) {
            return -1;
        }
    }

    return 0;
}

static void reds_cleanup(RedsState *reds)
{
#ifdef RED_STATISTICS
    stat_file_unlink(reds->stat_file);
#endif
}

SPICE_DESTRUCTOR_FUNC(reds_exit)
{
    GList *l;

    pthread_mutex_lock(&global_reds_lock);
    for (l = servers; l != NULL; l = l->next) {
        RedsState *reds = l->data;
        reds_cleanup(reds);
    }
    pthread_mutex_unlock(&global_reds_lock);
}

static inline void on_activating_ticketing(RedsState *reds)
{
    if (!reds->config->ticketing_enabled && reds_main_channel_connected(reds)) {
        spice_warning("disconnecting");
        reds_disconnect(reds);
    }
}

static void reds_config_set_image_compression(RedsState *reds, SpiceImageCompression val)
{
    if (val == reds->config->image_compression) {
        return;
    }
    reds->config->image_compression = val;
    reds_on_ic_change(reds);
}

static void reds_set_one_channel_security(RedsState *reds, int id, uint32_t security)
{
    ChannelSecurityOptions *security_options;

    if ((security_options = reds_find_channel_security(reds, id))) {
        security_options->options = security;
        return;
    }
    security_options = g_new(ChannelSecurityOptions, 1);
    security_options->channel_id = id;
    security_options->options = security;
    security_options->next = reds->config->channels_security;
    reds->config->channels_security = security_options;
}

#define REDS_SAVE_VERSION 1

static void reds_mig_release(RedServerConfig *config)
{
    if (config->mig_spice) {
        g_free(config->mig_spice->cert_subject);
        g_free(config->mig_spice->host);
        g_free(config->mig_spice);
        config->mig_spice = NULL;
    }
}

static void reds_mig_started(RedsState *reds)
{
    spice_debug("trace");
    spice_assert(reds->config->mig_spice);

    reds->mig_inprogress = TRUE;
    reds->mig_wait_connect = TRUE;
    reds_core_timer_start(reds, reds->mig_timer, MIGRATE_TIMEOUT);
}

static void reds_mig_fill_wait_disconnect(RedsState *reds)
{
    RedClient *client;

    spice_assert(reds->clients != NULL);
    /* tracking the clients, in order to ignore disconnection
     * of clients that got connected to the src after migration completion.*/
    GLIST_FOREACH(reds->clients, RedClient, client) {
        reds->mig_wait_disconnect_clients = g_list_append(reds->mig_wait_disconnect_clients, client);
    }
    reds->mig_wait_connect = FALSE;
    reds->mig_wait_disconnect = TRUE;
    reds_core_timer_start(reds, reds->mig_timer, MIGRATE_TIMEOUT);
}

static void reds_mig_cleanup_wait_disconnect(RedsState *reds)
{
    g_list_free(reds->mig_wait_disconnect_clients);
    reds->mig_wait_disconnect = FALSE;
}

static void reds_mig_remove_wait_disconnect_client(RedsState *reds, RedClient *client)
{
    g_warn_if_fail(g_list_find(reds->mig_wait_disconnect_clients, client) != NULL);

    reds->mig_wait_disconnect_clients = g_list_remove(reds->mig_wait_disconnect_clients, client);
    if (reds->mig_wait_disconnect_clients == NULL) {
       reds_mig_cleanup(reds);
    }
}

static void reds_migrate_channels_seamless(RedsState *reds)
{
    RedClient *client;

    /* seamless migration is supported for only one client for now */
    client = reds_get_client(reds);
    red_client_migrate(client);
}

static void reds_mig_finished(RedsState *reds, int completed)
{
    spice_debug("trace");

    reds->mig_inprogress = TRUE;

    if (reds->src_do_seamless_migrate && completed) {
        reds_migrate_channels_seamless(reds);
    } else {
        main_channel_migrate_src_complete(reds->main_channel, completed);
    }

    if (completed) {
        reds_mig_fill_wait_disconnect(reds);
    } else {
        reds_mig_cleanup(reds);
    }
    reds_mig_release(reds->config);
}

static void reds_mig_switch(RedsState *reds)
{
    if (!reds->config->mig_spice) {
        spice_warning("reds_mig_switch called without migrate_info set");
        return;
    }
    main_channel_migrate_switch(reds->main_channel, reds->config->mig_spice);
    reds_mig_release(reds->config);
}

static void migrate_timeout(void *opaque)
{
    RedsState *reds = opaque;
    spice_debug("trace");
    spice_assert(reds->mig_wait_connect || reds->mig_wait_disconnect);
    if (reds->mig_wait_connect) {
        /* we will fall back to the switch host scheme when migration completes */
        main_channel_migrate_cancel_wait(reds->main_channel);
        /* in case part of the client haven't yet completed the previous migration, disconnect them */
        reds_mig_target_client_disconnect_all(reds);
        reds_mig_cleanup(reds);
    } else {
        reds_mig_disconnect(reds);
    }
}

uint32_t reds_get_mm_time(void)
{
    return spice_get_monotonic_time_ns() / NSEC_PER_MILLISEC;
}

void reds_enable_mm_time(RedsState *reds)
{
    reds->mm_time_enabled = TRUE;
    reds->mm_time_latency = MM_TIME_DELTA;
    reds_send_mm_time(reds);
}

void reds_disable_mm_time(RedsState *reds)
{
    reds->mm_time_enabled = FALSE;
}

static RedCharDevice *attach_to_red_agent(RedsState *reds, SpiceCharDeviceInstance *sin)
{
    RedCharDeviceVDIPort *dev = reds->agent_dev;
    SpiceCharDeviceInterface *sif;

    dev->priv->agent_attached = TRUE;
    red_char_device_reset_dev_instance(RED_CHAR_DEVICE(dev), sin);

    reds->vdagent = sin;
    reds_update_mouse_mode(reds);

    sif = spice_char_device_get_interface(reds->vdagent);
    if (sif->state) {
        sif->state(reds->vdagent, 1);
    }

    if (!reds_main_channel_connected(reds)) {
        return RED_CHAR_DEVICE(dev);
    }

    dev->priv->read_filter.discard_all = FALSE;
    dev->priv->plug_generation++;

    if (dev->priv->mig_data ||
        red_channel_is_waiting_for_migrate_data(RED_CHANNEL(reds->main_channel))) {
        /* Migration in progress (code is running on the destination host):
         * 1.  Add the client to spice char device, if it was not already added.
         * 2.a If this (qemu-kvm state load side of migration) happens first
         *     then wait for spice migration data to arrive. Otherwise
         * 2.b If this happens second ==> we already have spice migrate data
         *     then restore state
         */
        if (!red_char_device_client_exists(RED_CHAR_DEVICE(dev), reds_get_client(reds))) {
            int client_added;

            client_added = red_char_device_client_add(RED_CHAR_DEVICE(dev),
                                                      reds_get_client(reds),
                                                      TRUE, /* flow control */
                                                      REDS_VDI_PORT_NUM_RECEIVE_BUFFS,
                                                      REDS_AGENT_WINDOW_SIZE,
                                                      ~0,
                                                      TRUE);

            if (!client_added) {
                spice_warning("failed to add client to agent");
                reds_disconnect(reds);
            }
        }

        if (dev->priv->mig_data) {
            spice_debug("restoring dev from stored migration data");
            spice_assert(dev->priv->plug_generation == 1);
            reds_agent_state_restore(reds, dev->priv->mig_data);
            g_free(dev->priv->mig_data);
            dev->priv->mig_data = NULL;
        }
        else {
            spice_debug("waiting for migration data");
        }
    } else {
        /* we will associate the client with the char device, upon reds_on_main_agent_start,
         * in response to MSGC_AGENT_START */
        main_channel_push_agent_connected(reds->main_channel);
    }

    return RED_CHAR_DEVICE(dev);
}

SPICE_GNUC_VISIBLE void spice_server_char_device_wakeup(SpiceCharDeviceInstance* sin)
{
    if (!sin->st) {
        spice_warning("no RedCharDevice attached to instance %p", sin);
        return;
    }
    red_char_device_wakeup(sin->st);
}

#define SUBTYPE_VDAGENT "vdagent"
#define SUBTYPE_SMARTCARD "smartcard"
#define SUBTYPE_USBREDIR "usbredir"
#define SUBTYPE_PORT "port"

static const char *const spice_server_char_device_recognized_subtypes_list[] = {
    SUBTYPE_VDAGENT,
#ifdef USE_SMARTCARD
    SUBTYPE_SMARTCARD,
#endif
    SUBTYPE_USBREDIR,
    NULL,
};

SPICE_GNUC_VISIBLE const char** spice_server_char_device_recognized_subtypes(void)
{
    return (const char **) spice_server_char_device_recognized_subtypes_list;
}

static void reds_add_char_device(RedsState *reds, RedCharDevice *dev)
{
    reds->char_devices = g_list_append(reds->char_devices, dev);
}

static void reds_on_char_device_destroy(RedsState *reds,
                                        RedCharDevice *dev)
{
    g_return_if_fail(reds != NULL);
    g_warn_if_fail(g_list_find(reds->char_devices, dev) != NULL);

    reds->char_devices = g_list_remove(reds->char_devices, dev);
}

static int spice_server_char_device_add_interface(SpiceServer *reds,
                                           SpiceBaseInstance *sin)
{
    SpiceCharDeviceInstance* char_device =
            SPICE_UPCAST(SpiceCharDeviceInstance, sin);
    RedCharDevice *dev_state = NULL;

    spice_debug("CHAR_DEVICE %s", char_device->subtype);
    if (strcmp(char_device->subtype, SUBTYPE_VDAGENT) == 0) {
        if (reds->vdagent) {
            spice_warning("vdagent already attached");
            return -1;
        }
        dev_state = attach_to_red_agent(reds, char_device);
        g_object_ref(dev_state);
    }
#ifdef USE_SMARTCARD
    else if (strcmp(char_device->subtype, SUBTYPE_SMARTCARD) == 0) {
        if (!(dev_state = smartcard_device_connect(reds, char_device))) {
            return -1;
        }
    }
#endif
    else if (strcmp(char_device->subtype, SUBTYPE_USBREDIR) == 0) {
        dev_state = spicevmc_device_connect(reds, char_device, SPICE_CHANNEL_USBREDIR);
    }
    else if (strcmp(char_device->subtype, SUBTYPE_PORT) == 0) {
        if (strcmp(char_device->portname, "org.spice-space.webdav.0") == 0) {
            dev_state = spicevmc_device_connect(reds, char_device, SPICE_CHANNEL_WEBDAV);
        } else if (strcmp(char_device->portname, "org.spice-space.stream.0") == 0) {
            dev_state = RED_CHAR_DEVICE(stream_device_connect(reds, char_device));
        } else {
            dev_state = spicevmc_device_connect(reds, char_device, SPICE_CHANNEL_PORT);
        }
    }

    if (dev_state) {
        /* When spicevmc_device_connect() is called to create a RedCharDevice,
         * it also assigns that as the internal state for char_device. This is
         * just a sanity check to ensure that assumption is correct */
        spice_assert(dev_state == char_device->st);

        g_object_weak_ref(G_OBJECT(dev_state),
                          (GWeakNotify)reds_on_char_device_destroy,
                          reds);
        /* setting the char_device state to "started" for backward compatibily with
         * qemu releases that don't call spice api for start/stop (not implemented yet) */
        if (reds->vm_running) {
            red_char_device_start(dev_state);
        }
        reds_add_char_device(reds, dev_state);
    } else {
        spice_warning("failed to create device state for %s", char_device->subtype);
        return -1;
    }
    return 0;
}

static int spice_server_char_device_remove_interface(RedsState *reds, SpiceBaseInstance *sin)
{
    SpiceCharDeviceInstance* char_device =
            SPICE_UPCAST(SpiceCharDeviceInstance, sin);

    spice_debug("remove CHAR_DEVICE %s", char_device->subtype);
    if (strcmp(char_device->subtype, SUBTYPE_VDAGENT) == 0) {
        g_return_val_if_fail(char_device == reds->vdagent, -1);
        if (reds->vdagent) {
            reds_agent_remove(reds);
            red_char_device_reset_dev_instance(RED_CHAR_DEVICE(reds->agent_dev), NULL);
        }
    }
#ifdef USE_SMARTCARD
    else if (strcmp(char_device->subtype, SUBTYPE_SMARTCARD) == 0) {
        smartcard_device_disconnect(char_device);
    }
#endif
    else if (strcmp(char_device->subtype, SUBTYPE_USBREDIR) == 0 ||
             strcmp(char_device->subtype, SUBTYPE_PORT) == 0) {
        spicevmc_device_disconnect(reds, char_device);
    } else {
        spice_warning("failed to remove char device %s", char_device->subtype);
    }

    char_device->st = NULL;
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_add_interface(SpiceServer *reds,
                                                  SpiceBaseInstance *sin)
{
    const SpiceBaseInterface *interface = sin->sif;

    if (strcmp(interface->type, SPICE_INTERFACE_KEYBOARD) == 0) {
        spice_debug("SPICE_INTERFACE_KEYBOARD");
        if (interface->major_version != SPICE_INTERFACE_KEYBOARD_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_KEYBOARD_MINOR) {
            spice_warning("unsupported keyboard interface");
            return -1;
        }
        if (inputs_channel_set_keyboard(reds->inputs_channel,
                                        SPICE_UPCAST(SpiceKbdInstance, sin)) != 0) {
            return -1;
        }
    } else if (strcmp(interface->type, SPICE_INTERFACE_MOUSE) == 0) {
        spice_debug("SPICE_INTERFACE_MOUSE");
        if (interface->major_version != SPICE_INTERFACE_MOUSE_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_MOUSE_MINOR) {
            spice_warning("unsupported mouse interface");
            return -1;
        }
        if (inputs_channel_set_mouse(reds->inputs_channel,
                                     SPICE_UPCAST(SpiceMouseInstance, sin)) != 0) {
            return -1;
        }
    } else if (strcmp(interface->type, SPICE_INTERFACE_QXL) == 0) {
        QXLInstance *qxl;

        spice_debug("SPICE_INTERFACE_QXL");
        if (interface->major_version != SPICE_INTERFACE_QXL_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_QXL_MINOR) {
            spice_warning("unsupported qxl interface");
            return -1;
        }

        qxl = SPICE_UPCAST(QXLInstance, sin);
        if (qxl->id < 0) {
            spice_warning("invalid QXL ID");
            return -1;
        }
        if (reds_find_channel(reds, SPICE_CHANNEL_DISPLAY, qxl->id)) {
            spice_warning("QXL ID already allocated");
            return -1;
        }
        red_qxl_init(reds, qxl);
        reds->qxl_instances = g_list_prepend(reds->qxl_instances, qxl);

        /* this function has to be called after the qxl is on the list
         * as QXLInstance clients expect the qxl to be on the list when
         * this callback is called. This as clients assume they can start the
         * qxl_instances. Also note that this should be the first callback to
         * be called. */
        red_qxl_attach_worker(qxl);
        red_qxl_set_compression_level(qxl, calc_compression_level(reds));
    } else if (strcmp(interface->type, SPICE_INTERFACE_TABLET) == 0) {
        SpiceTabletInstance *tablet = SPICE_UPCAST(SpiceTabletInstance, sin);
        spice_debug("SPICE_INTERFACE_TABLET");
        if (interface->major_version != SPICE_INTERFACE_TABLET_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_TABLET_MINOR) {
            spice_warning("unsupported tablet interface");
            return -1;
        }
        if (inputs_channel_set_tablet(reds->inputs_channel, tablet) != 0) {
            return -1;
        }
        reds_update_mouse_mode(reds);
        if (reds->is_client_mouse_allowed) {
            inputs_channel_set_tablet_logical_size(reds->inputs_channel, reds->monitor_mode.x_res, reds->monitor_mode.y_res);
        }

    } else if (strcmp(interface->type, SPICE_INTERFACE_PLAYBACK) == 0) {
        spice_debug("SPICE_INTERFACE_PLAYBACK");
        if (interface->major_version != SPICE_INTERFACE_PLAYBACK_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_PLAYBACK_MINOR) {
            spice_warning("unsupported playback interface");
            return -1;
        }
        snd_attach_playback(reds, SPICE_UPCAST(SpicePlaybackInstance, sin));

    } else if (strcmp(interface->type, SPICE_INTERFACE_RECORD) == 0) {
        spice_debug("SPICE_INTERFACE_RECORD");
        if (interface->major_version != SPICE_INTERFACE_RECORD_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_RECORD_MINOR) {
            spice_warning("unsupported record interface");
            return -1;
        }
        snd_attach_record(reds, SPICE_UPCAST(SpiceRecordInstance, sin));

    } else if (strcmp(interface->type, SPICE_INTERFACE_CHAR_DEVICE) == 0) {
        if (interface->major_version != SPICE_INTERFACE_CHAR_DEVICE_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_CHAR_DEVICE_MINOR) {
            spice_warning("unsupported char device interface");
            return -1;
        }
        spice_server_char_device_add_interface(reds, sin);

    } else if (strcmp(interface->type, SPICE_INTERFACE_MIGRATION) == 0) {
        spice_debug("SPICE_INTERFACE_MIGRATION");
        if (reds->migration_interface) {
            spice_warning("already have migration");
            return -1;
        }

        if (interface->major_version != SPICE_INTERFACE_MIGRATION_MAJOR ||
            interface->minor_version > SPICE_INTERFACE_MIGRATION_MINOR) {
            spice_warning("unsupported migration interface");
            return -1;
        }
        reds->migration_interface = SPICE_UPCAST(SpiceMigrateInstance, sin);
        reds->migration_interface->st = (SpiceMigrateState *)(intptr_t)1; // dummy pointer
    }

    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_remove_interface(SpiceBaseInstance *sin)
{
    RedsState *reds;
    const SpiceBaseInterface *interface;

    g_return_val_if_fail(sin != NULL, -1);

    interface = sin->sif;
    if (strcmp(interface->type, SPICE_INTERFACE_TABLET) == 0) {
        SpiceTabletInstance *tablet = SPICE_UPCAST(SpiceTabletInstance, sin);
        g_return_val_if_fail(tablet->st != NULL, -1);
        reds = spice_tablet_state_get_server(tablet->st);
        spice_debug("remove SPICE_INTERFACE_TABLET");
        inputs_channel_detach_tablet(reds->inputs_channel, tablet);
        reds_update_mouse_mode(reds);
    } else if (strcmp(interface->type, SPICE_INTERFACE_PLAYBACK) == 0) {
        spice_debug("remove SPICE_INTERFACE_PLAYBACK");
        snd_detach_playback(SPICE_UPCAST(SpicePlaybackInstance, sin));
    } else if (strcmp(interface->type, SPICE_INTERFACE_RECORD) == 0) {
        spice_debug("remove SPICE_INTERFACE_RECORD");
        snd_detach_record(SPICE_UPCAST(SpiceRecordInstance, sin));
    } else if (strcmp(interface->type, SPICE_INTERFACE_CHAR_DEVICE) == 0) {
        SpiceCharDeviceInstance *char_device = SPICE_UPCAST(SpiceCharDeviceInstance, sin);
        g_return_val_if_fail(char_device->st != NULL, -1);
        reds = red_char_device_get_server(char_device->st);
        return spice_server_char_device_remove_interface(reds, sin);
    } else if (strcmp(interface->type, SPICE_INTERFACE_QXL) == 0) {
        QXLInstance *qxl;

        qxl = SPICE_UPCAST(QXLInstance, sin);
        g_return_val_if_fail(qxl->st != NULL, -1);
        reds = red_qxl_get_server(qxl->st);
        reds->qxl_instances = g_list_remove(reds->qxl_instances, qxl);
        red_qxl_destroy(qxl);
    } else {
        spice_warning("VD_INTERFACE_REMOVING unsupported");
        return -1;
    }

    return 0;
}

static int do_spice_init(RedsState *reds, SpiceCoreInterface *core_interface)
{
    spice_debug("starting %s", VERSION);

    if (core_interface->base.major_version != SPICE_INTERFACE_CORE_MAJOR) {
        spice_warning("bad core interface version");
        goto err;
    }
    reds->core = core_interface_adapter;
    reds->core.public_interface = core_interface;
    reds->agent_dev = red_char_device_vdi_port_new(reds);
    reds_update_agent_properties(reds);
    reds->clients = NULL;
    reds->main_dispatcher = main_dispatcher_new(reds);
    reds->channels = NULL;
    reds->mig_target_clients = NULL;
    reds->char_devices = NULL;
    reds->mig_wait_disconnect_clients = NULL;
    reds->vm_running = TRUE; /* for backward compatibility */

    if (!(reds->mig_timer = reds->core.timer_add(&reds->core, migrate_timeout, reds))) {
        spice_error("migration timer create failed");
    }

    if (reds_init_net(reds) < 0) {
        spice_warning("Failed to open SPICE sockets");
        goto err;
    }
    if (reds->secure_listen_socket != -1) {
        if (reds_init_ssl(reds) < 0) {
            goto err;
        }
    }
#if HAVE_SASL
    int saslerr;
    if ((saslerr = sasl_server_init(NULL, reds->config->sasl_appname ?
                                    reds->config->sasl_appname : "spice")) != SASL_OK) {
        spice_error("Failed to initialize SASL auth %s",
                  sasl_errstring(saslerr, NULL, NULL));
        goto err;
    }
#endif

    reds->main_channel = main_channel_new(reds);
    reds->inputs_channel = inputs_channel_new(reds);

    reds->mouse_mode = SPICE_MOUSE_MODE_SERVER;

    spice_buffer_free(&reds->client_monitors_config);

    reds->allow_multiple_clients = getenv(SPICE_DEBUG_ALLOW_MC_ENV) != NULL;
    if (reds->allow_multiple_clients) {
        spice_warning("spice: allowing multiple client connections");
    }
    pthread_mutex_lock(&global_reds_lock);
    servers = g_list_prepend(servers, reds);
    pthread_mutex_unlock(&global_reds_lock);
    return 0;

err:
    reds_cleanup_net(reds);
    return -1;
}

static const char default_renderer[] = "sw";
#if defined(HAVE_GSTREAMER_1_0) || defined(HAVE_GSTREAMER_0_10)
#define GSTREAMER_CODECS "gstreamer:mjpeg;gstreamer:h264;gstreamer:vp8;gstreamer:vp9;"
#else
#define GSTREAMER_CODECS ""
#endif
static const char default_video_codecs[] = "spice:mjpeg;" GSTREAMER_CODECS;

/* new interface */
SPICE_GNUC_VISIBLE SpiceServer *spice_server_new(void)
{
    const char *record_filename;
    RedsState *reds = g_new0(RedsState, 1);

    reds->config = g_new0(RedServerConfig, 1);
    reds->config->default_channel_security =
        SPICE_CHANNEL_SECURITY_NONE | SPICE_CHANNEL_SECURITY_SSL;
    reds->config->renderers = g_array_sized_new(FALSE, TRUE, sizeof(uint32_t), RED_RENDERER_LAST);
    reds->config->spice_port = -1;
    reds->config->spice_secure_port = -1;
    reds->config->spice_listen_socket_fd = -1;
    reds->config->spice_family = PF_UNSPEC;
    reds->config->sasl_enabled = 0; // sasl disabled by default
#if HAVE_SASL
    reds->config->sasl_appname = NULL; // default to "spice" if NULL
#endif
    reds->config->spice_uuid_is_set = FALSE;
    memset(reds->config->spice_uuid, 0, sizeof(reds->config->spice_uuid));
    reds->config->ticketing_enabled = TRUE; /* ticketing enabled by default */
    reds->config->streaming_video = SPICE_STREAM_VIDEO_FILTER;
    reds->config->video_codecs = g_array_new(FALSE, FALSE, sizeof(RedVideoCodec));
    reds->config->image_compression = SPICE_IMAGE_COMPRESSION_AUTO_GLZ;
    reds->config->playback_compression = TRUE;
    reds->config->jpeg_state = SPICE_WAN_COMPRESSION_AUTO;
    reds->config->zlib_glz_state = SPICE_WAN_COMPRESSION_AUTO;
    reds->config->agent_mouse = TRUE;
    reds->config->agent_copypaste = TRUE;
    reds->config->agent_file_xfer = TRUE;
    reds->config->exit_on_disconnect = FALSE;
#ifdef RED_STATISTICS
    reds->stat_file = stat_file_new(REDS_MAX_STAT_NODES);
    /* Create an initial node. This will be the 0 node making easier
     * to initialize node references.
     */
    stat_file_add_node(reds->stat_file, INVALID_STAT_REF, "default_channel", TRUE);
#endif
    reds->listen_socket = -1;
    reds->secure_listen_socket = -1;

    /* This environment was in red-worker so the "WORKER" in it.
     * For compatibility reason we maintain the old name */
    record_filename = getenv("SPICE_WORKER_RECORD_FILENAME");
    if (record_filename) {
        reds->record = red_record_new(record_filename);
    }
    return reds;
}

typedef struct {
    uint32_t id;
    const char *name;
} EnumNames;

static gboolean get_name_index(const EnumNames names[], const char *name, uint32_t *index)
{
    if (name) {
        int i;
        for (i = 0; names[i].name; i++) {
            if (strcmp(name, names[i].name) == 0) {
                *index = i;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static const EnumNames renderer_names[] = {
    {RED_RENDERER_SW, "sw"},
    {RED_RENDERER_INVALID, NULL},
};

static gboolean reds_add_renderer(RedsState *reds, const char *name)
{
    uint32_t index;

    if (reds->config->renderers->len == RED_RENDERER_LAST ||
        !get_name_index(renderer_names, name, &index)) {
        return FALSE;
    }
    g_array_append_val(reds->config->renderers, renderer_names[index].id);
    return TRUE;
}

static const EnumNames video_encoder_names[] = {
    {0, "spice"},
    {1, "gstreamer"},
    {0, NULL},
};

static const new_video_encoder_t video_encoder_procs[] = {
    &mjpeg_encoder_new,
#if defined(HAVE_GSTREAMER_1_0) || defined(HAVE_GSTREAMER_0_10)
    &gstreamer_encoder_new,
#else
    NULL,
#endif
};

static const EnumNames video_codec_names[] = {
    {SPICE_VIDEO_CODEC_TYPE_MJPEG, "mjpeg"},
    {SPICE_VIDEO_CODEC_TYPE_VP8, "vp8"},
    {SPICE_VIDEO_CODEC_TYPE_H264, "h264"},
    {SPICE_VIDEO_CODEC_TYPE_VP9, "vp9"},
    {0, NULL},
};

static const int video_codec_caps[] = {
    SPICE_DISPLAY_CAP_CODEC_MJPEG,
    SPICE_DISPLAY_CAP_CODEC_VP8,
    SPICE_DISPLAY_CAP_CODEC_H264,
    SPICE_DISPLAY_CAP_CODEC_VP9,
};


/* Parses the given codec string and returns newly-allocated strings describing
 * the next encoder and codec in the list. These strings must be freed by the
 * caller.
 *
 * @codecs: a codec string in the following format: encoder:codec;encoder:codec
 * @encoder: a location to return the parsed encoder
 * @codec: a location to return the parsed codec
 * @return the position of the next codec in the string
 */
static const char* parse_next_video_codec(const char *codecs, char **encoder,
                                          char **codec)
{
    if (!codecs) {
        return NULL;
    }
    codecs += strspn(codecs, ";");
    if (!*codecs) {
        return NULL;
    }
    int n;
    *encoder = *codec = NULL;
    if (sscanf(codecs, "%m[0-9a-zA-Z_]:%m[0-9a-zA-Z_]%n", encoder, codec, &n) == 2) {
        // this avoids accepting "encoder:codec" followed by garbage like "$%*"
        if (codecs[n] != ';' && codecs[n] != '\0') {
            free(*codec);
            *codec = NULL;
        }
    }
    return codecs + strcspn(codecs, ";");
}

static void reds_set_video_codecs_from_string(RedsState *reds, const char *codecs)
{
    char *encoder_name, *codec_name;
    GArray *video_codecs;

    g_return_if_fail(codecs != NULL);

    if (strcmp(codecs, "auto") == 0) {
        codecs = default_video_codecs;
    }

    video_codecs = g_array_new(FALSE, FALSE, sizeof(RedVideoCodec));
    const char *c = codecs;
    while ( (c = parse_next_video_codec(c, &encoder_name, &codec_name)) ) {
        uint32_t encoder_index, codec_index;
        if (!encoder_name || !codec_name) {
            spice_warning("spice: invalid encoder:codec value at %s", codecs);

        } else if (!get_name_index(video_encoder_names, encoder_name, &encoder_index)){
            spice_warning("spice: unknown video encoder %s", encoder_name);

        } else if (!get_name_index(video_codec_names, codec_name, &codec_index)) {
            spice_warning("spice: unknown video codec %s", codec_name);

        } else if (!video_encoder_procs[encoder_index]) {
            spice_warning("spice: unsupported video encoder %s", encoder_name);

        } else {
            RedVideoCodec new_codec;
            new_codec.create = video_encoder_procs[encoder_index];
            new_codec.type = video_codec_names[codec_index].id;
            new_codec.cap = video_codec_caps[codec_index];
            g_array_append_val(video_codecs, new_codec);
        }

        /* these are allocated by sscanf, do not use g_free */
        free(encoder_name);
        free(codec_name);
        codecs = c;
    }

    if (video_codecs->len == 0) {
        spice_warning("Failed to set video codecs, input string: '%s'", codecs);
        g_array_unref(video_codecs);
        return;
    }

    reds_set_video_codecs(reds, video_codecs);
}

SPICE_GNUC_VISIBLE int spice_server_init(SpiceServer *reds, SpiceCoreInterface *core)
{
    int ret;

    ret = do_spice_init(reds, core);
    if (reds->config->renderers->len == 0) {
        reds_add_renderer(reds, default_renderer);
    }
    if (reds->config->video_codecs->len == 0) {
        reds_set_video_codecs_from_string(reds, default_video_codecs);
    }
    return ret;
}

static void reds_config_free(RedServerConfig *config)
{
    ChannelSecurityOptions *curr, *next;

    reds_mig_release(config);
    for (curr = config->channels_security; curr; curr = next) {
        next = curr->next;
        g_free(curr);
    }
#if HAVE_SASL
    g_free(config->sasl_appname);
#endif
    g_free(config->spice_name);
    g_array_unref(config->renderers);
    g_array_unref(config->video_codecs);
    g_free(config);
}

SPICE_GNUC_VISIBLE void spice_server_destroy(SpiceServer *reds)
{
    /* remove the server from the list of servers so that we don't attempt to
     * free it again at exit */
    pthread_mutex_lock(&global_reds_lock);
    servers = g_list_remove(servers, reds);
    pthread_mutex_unlock(&global_reds_lock);

    g_list_free_full(reds->qxl_instances, (GDestroyNotify)red_qxl_destroy);

    if (reds->inputs_channel) {
        red_channel_destroy(RED_CHANNEL(reds->inputs_channel));
    }
    if (reds->main_channel) {
        red_channel_destroy(RED_CHANNEL(reds->main_channel));
    }
    reds_core_timer_remove(reds, reds->mig_timer);

    if (reds->ctx) {
        SSL_CTX_free(reds->ctx);
    }

    if (reds->main_dispatcher) {
        g_object_unref(reds->main_dispatcher);
    }
    reds_cleanup_net(reds);
    g_clear_object(&reds->agent_dev);

    // NOTE: don't replace with g_list_free_full as this function that passed callback
    // don't change the list while g_object_unref in this case will change it.
    RedCharDevice *dev;
    GLIST_FOREACH(reds->char_devices, RedCharDevice, dev) {
        g_object_unref(dev);
    }
    g_list_free(reds->char_devices);
    reds->char_devices = NULL;

    g_list_free(reds->channels);
    reds->channels = NULL;

    spice_buffer_free(&reds->client_monitors_config);
    red_record_unref(reds->record);
    reds_cleanup(reds);
#ifdef RED_STATISTICS
    stat_file_free(reds->stat_file);
#endif

    reds_config_free(reds->config);
    g_free(reds);
}

SPICE_GNUC_VISIBLE spice_compat_version_t spice_get_current_compat_version(void)
{
    return SPICE_COMPAT_VERSION_CURRENT;
}

SPICE_GNUC_VISIBLE int spice_server_set_compat_version(SpiceServer *reds,
                                                       spice_compat_version_t version)
{
    if (version < SPICE_COMPAT_VERSION_0_6) {
        /* We don't support 0.4 compat mode atm */
        return -1;
    }

    if (version > SPICE_COMPAT_VERSION_CURRENT) {
        /* Not compatible with future versions */
        return -1;
    }
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_port(SpiceServer *reds, int port)
{
    if (port < 0 || port > 0xffff) {
        return -1;
    }
    reds->config->spice_port = port;
    return 0;
}

SPICE_GNUC_VISIBLE void spice_server_set_addr(SpiceServer *reds, const char *addr, int flags)
{
    g_strlcpy(reds->config->spice_addr, addr, sizeof(reds->config->spice_addr));

    if (flags == SPICE_ADDR_FLAG_IPV4_ONLY) {
        reds->config->spice_family = PF_INET;
    } else if (flags == SPICE_ADDR_FLAG_IPV6_ONLY) {
        reds->config->spice_family = PF_INET6;
    } else if (flags == SPICE_ADDR_FLAG_UNIX_ONLY) {
        reds->config->spice_family = AF_UNIX;
    } else if (flags != 0) {
        spice_warning("unknown address flag: 0x%X", flags);
    }
}

SPICE_GNUC_VISIBLE int spice_server_set_listen_socket_fd(SpiceServer *s, int listen_fd)
{
    s->config->spice_listen_socket_fd = listen_fd;
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_exit_on_disconnect(SpiceServer *s, int flag)
{
    s->config->exit_on_disconnect = !!flag;
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_noauth(SpiceServer *s)
{
    memset(s->config->taTicket.password, 0, sizeof(s->config->taTicket.password));
    s->config->ticketing_enabled = FALSE;
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_sasl(SpiceServer *s, int enabled)
{
#if HAVE_SASL
    s->config->sasl_enabled = enabled;
    return 0;
#else
    return -1;
#endif
}

SPICE_GNUC_VISIBLE int spice_server_set_sasl_appname(SpiceServer *s, const char *appname)
{
#if HAVE_SASL
    g_free(s->config->sasl_appname);
    s->config->sasl_appname = g_strdup(appname);
    return 0;
#else
    return -1;
#endif
}

SPICE_GNUC_VISIBLE void spice_server_set_name(SpiceServer *s, const char *name)
{
    g_free(s->config->spice_name);
    s->config->spice_name = g_strdup(name);
}

SPICE_GNUC_VISIBLE void spice_server_set_uuid(SpiceServer *s, const uint8_t uuid[16])
{
    memcpy(s->config->spice_uuid, uuid, sizeof(s->config->spice_uuid));
    s->config->spice_uuid_is_set = TRUE;
}

SPICE_GNUC_VISIBLE int spice_server_set_ticket(SpiceServer *reds,
                                               const char *passwd, int lifetime,
                                               int fail_if_connected,
                                               int disconnect_if_connected)
{
    if (reds_main_channel_connected(reds)) {
        if (fail_if_connected) {
            return -1;
        }
        if (disconnect_if_connected) {
            reds_disconnect(reds);
        }
    }

    on_activating_ticketing(reds);
    reds->config->ticketing_enabled = TRUE;
    if (lifetime == 0) {
        reds->config->taTicket.expiration_time = INT_MAX;
    } else {
        time_t now = spice_get_monotonic_time_ns() / NSEC_PER_SEC;
        reds->config->taTicket.expiration_time = now + lifetime;
    }
    if (passwd != NULL) {
        if (strlen(passwd) > SPICE_MAX_PASSWORD_LENGTH)
            return -1;
        g_strlcpy(reds->config->taTicket.password, passwd, sizeof(reds->config->taTicket.password));
    } else {
        memset(reds->config->taTicket.password, 0, sizeof(reds->config->taTicket.password));
        reds->config->taTicket.expiration_time = 0;
    }
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_tls(SpiceServer *s, int port,
                                            const char *ca_cert_file, const char *certs_file,
                                            const char *private_key_file, const char *key_passwd,
                                            const char *dh_key_file, const char *ciphersuite)
{
    if (port == 0 || ca_cert_file == NULL || certs_file == NULL ||
        private_key_file == NULL) {
        return -1;
    }
    if (port < 0 || port > 0xffff) {
        return -1;
    }
    memset(&s->config->ssl_parameters, 0, sizeof(s->config->ssl_parameters));

    s->config->spice_secure_port = port;
    g_strlcpy(s->config->ssl_parameters.ca_certificate_file, ca_cert_file,
              sizeof(s->config->ssl_parameters.ca_certificate_file));
    g_strlcpy(s->config->ssl_parameters.certs_file, certs_file,
              sizeof(s->config->ssl_parameters.certs_file));
    g_strlcpy(s->config->ssl_parameters.private_key_file, private_key_file,
              sizeof(s->config->ssl_parameters.private_key_file));

    if (key_passwd) {
        g_strlcpy(s->config->ssl_parameters.keyfile_password, key_passwd,
                  sizeof(s->config->ssl_parameters.keyfile_password));
    }
    if (ciphersuite) {
        g_strlcpy(s->config->ssl_parameters.ciphersuite, ciphersuite,
                  sizeof(s->config->ssl_parameters.ciphersuite));
    }
    if (dh_key_file) {
        g_strlcpy(s->config->ssl_parameters.dh_key_file, dh_key_file,
                  sizeof(s->config->ssl_parameters.dh_key_file));
    }
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_image_compression(SpiceServer *s,
                                                          SpiceImageCompression comp)
{
#ifndef USE_LZ4
    if (comp == SPICE_IMAGE_COMPRESSION_LZ4) {
        spice_warning("LZ4 compression not supported, falling back to auto GLZ");
        comp = SPICE_IMAGE_COMPRESSION_AUTO_GLZ;
        reds_config_set_image_compression(s, comp);
        return -1;
    }
#endif
    reds_config_set_image_compression(s, comp);
    return 0;
}

SPICE_GNUC_VISIBLE SpiceImageCompression spice_server_get_image_compression(SpiceServer *s)
{
    return s->config->image_compression;
}

SPICE_GNUC_VISIBLE int spice_server_set_jpeg_compression(SpiceServer *s, spice_wan_compression_t comp)
{
    if (comp == SPICE_WAN_COMPRESSION_INVALID) {
        spice_error("invalid jpeg state");
        return -1;
    }
    // todo: support dynamically changing the state
    s->config->jpeg_state = comp;
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_zlib_glz_compression(SpiceServer *s, spice_wan_compression_t comp)
{
    if (comp == SPICE_WAN_COMPRESSION_INVALID) {
        spice_error("invalid zlib_glz state");
        return -1;
    }
    // todo: support dynamically changing the state
    s->config->zlib_glz_state = comp;
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_channel_security(SpiceServer *s, const char *channel, int security)
{
    int type;
    if (channel == NULL) {
        s->config->default_channel_security = security;
        return 0;
    }
    type = red_channel_name_to_type(channel);
#ifndef USE_SMARTCARD
    if (type == SPICE_CHANNEL_SMARTCARD) {
        type = -1;
    }
#endif
    if (type == -1) {
        return -1;
    }

    reds_set_one_channel_security(s, type, security);
    return 0;
}

/* very obsolete and old function, retain only for ABI */
SPICE_GNUC_VISIBLE int spice_server_get_sock_info(SpiceServer *reds, struct sockaddr *sa, socklen_t *salen)
{
    return -1;
}

/* very obsolete and old function, retain only for ABI */
SPICE_GNUC_VISIBLE int spice_server_get_peer_info(SpiceServer *reds, struct sockaddr *sa, socklen_t *salen)
{
    return -1;
}

SPICE_GNUC_VISIBLE int spice_server_is_server_mouse(SpiceServer *reds)
{
    return reds->mouse_mode == SPICE_MOUSE_MODE_SERVER;
}

SPICE_GNUC_VISIBLE int spice_server_add_renderer(SpiceServer *reds, const char *name)
{
    if (!reds_add_renderer(reds, name)) {
        return -1;
    }
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_streaming_video(SpiceServer *reds, int value)
{
    if (value != SPICE_STREAM_VIDEO_OFF &&
        value != SPICE_STREAM_VIDEO_ALL &&
        value != SPICE_STREAM_VIDEO_FILTER)
        return -1;
    reds->config->streaming_video = value;
    reds_on_sv_change(reds);
    return 0;
}

uint32_t reds_get_streaming_video(const RedsState *reds)
{
    return reds->config->streaming_video;
}

SPICE_GNUC_VISIBLE int spice_server_set_video_codecs(SpiceServer *reds, const char *video_codecs)
{
    reds_set_video_codecs_from_string(reds, video_codecs);
    reds_on_vc_change(reds);
    return 0;
}

GArray* reds_get_video_codecs(const RedsState *reds)
{
    return reds->config->video_codecs;
}

static void reds_set_video_codecs(RedsState *reds, GArray *video_codecs)
{
    /* The video_codecs array is immutable */
    g_clear_pointer(&reds->config->video_codecs, g_array_unref);

    spice_return_if_fail(video_codecs != NULL);

    reds->config->video_codecs = video_codecs;
}

SPICE_GNUC_VISIBLE int spice_server_set_playback_compression(SpiceServer *reds, int enable)
{
    reds->config->playback_compression = !!enable;
    snd_set_playback_compression(enable);
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_agent_mouse(SpiceServer *reds, int enable)
{
    reds->config->agent_mouse = enable;
    reds_update_mouse_mode(reds);
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_agent_copypaste(SpiceServer *reds, int enable)
{
    reds->config->agent_copypaste = enable;
    reds_update_agent_properties(reds);
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_set_agent_file_xfer(SpiceServer *reds, int enable)
{
    reds->config->agent_file_xfer = enable;
    reds_update_agent_properties(reds);
    return 0;
}

/* returns FALSE if info is invalid */
static int reds_set_migration_dest_info(RedsState *reds,
                                        const char* dest,
                                        int port, int secure_port,
                                        const char* cert_subject)
{
    RedsMigSpice *spice_migration = NULL;

    reds_mig_release(reds->config);
    if ((port == -1 && secure_port == -1) || !dest) {
        return FALSE;
    }

    spice_migration = g_new0(RedsMigSpice, 1);
    spice_migration->port = port;
    spice_migration->sport = secure_port;
    spice_migration->host = g_strdup(dest);
    if (cert_subject) {
        spice_migration->cert_subject = g_strdup(cert_subject);
    }

    reds->config->mig_spice = spice_migration;

    return TRUE;
}

/* semi-seamless client migration */
SPICE_GNUC_VISIBLE int spice_server_migrate_connect(SpiceServer *reds, const char* dest,
                                                    int port, int secure_port,
                                                    const char* cert_subject)
{
    SpiceMigrateInterface *sif;
    int try_seamless;

    spice_debug("trace");
    spice_assert(reds->migration_interface);

    if (reds->expect_migrate) {
        spice_debug("consecutive calls without migration. Canceling previous call");
        main_channel_migrate_src_complete(reds->main_channel, FALSE);
    }

    sif = SPICE_UPCAST(SpiceMigrateInterface, reds->migration_interface->base.sif);

    if (!reds_set_migration_dest_info(reds, dest, port, secure_port, cert_subject)) {
        sif->migrate_connect_complete(reds->migration_interface);
        return -1;
    }

    reds->expect_migrate = TRUE;

    /*
     * seamless migration support was added to the client after the support in
     * agent_connect_tokens, so there shouldn't be contradicition - if
     * the client is capable of seamless migration, it is capbable of agent_connected_tokens.
     * The demand for agent_connected_tokens support is in order to assure that if migration
     * occured when the agent was not connected, the tokens state after migration will still
     * be valid (see reds_reset_vdp for more details).
     */
    try_seamless = reds->seamless_migration_enabled &&
                   red_channel_test_remote_cap(RED_CHANNEL(reds->main_channel),
                   SPICE_MAIN_CAP_AGENT_CONNECTED_TOKENS);
    /* main channel will take care of clients that are still during migration (at target)*/
    if (main_channel_migrate_connect(reds->main_channel, reds->config->mig_spice,
                                     try_seamless)) {
        reds_mig_started(reds);
    } else {
        if (reds->clients == NULL) {
            reds_mig_release(reds->config);
            spice_debug("no client connected");
        }
        sif->migrate_connect_complete(reds->migration_interface);
    }

    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_migrate_info(SpiceServer *reds, const char* dest,
                                          int port, int secure_port,
                                          const char* cert_subject)
{
    spice_debug("trace");
    spice_assert(!reds->migration_interface);

    if (!reds_set_migration_dest_info(reds, dest, port, secure_port, cert_subject)) {
        return -1;
    }
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_migrate_start(SpiceServer *reds)
{
    spice_debug("trace");
    if (!reds->config->mig_spice) {
        return -1;
    }
    return 0;
}

SPICE_GNUC_VISIBLE int spice_server_migrate_end(SpiceServer *reds, int completed)
{
    SpiceMigrateInterface *sif;
    int ret = 0;

    spice_debug("trace");

    spice_assert(reds->migration_interface);

    sif = SPICE_UPCAST(SpiceMigrateInterface, reds->migration_interface->base.sif);
    if (completed && !reds->expect_migrate && g_list_length(reds->clients) > 0) {
        spice_warning("spice_server_migrate_info was not called, disconnecting clients");
        reds_disconnect(reds);
        ret = -1;
        goto complete;
    }

    reds->expect_migrate = FALSE;
    if (!reds_main_channel_connected(reds)) {
        spice_debug("no peer connected");
        goto complete;
    }
    reds_mig_finished(reds, completed);
    return 0;
complete:
    if (sif->migrate_end_complete) {
        sif->migrate_end_complete(reds->migration_interface);
    }
    return ret;
}

/* interface for switch-host migration */
SPICE_GNUC_VISIBLE int spice_server_migrate_switch(SpiceServer *reds)
{
    spice_debug("trace");
    if (reds->clients == NULL) {
       return 0;
    }
    reds->expect_migrate = FALSE;
    reds_mig_switch(reds);
    return 0;
}

SPICE_GNUC_VISIBLE void spice_server_vm_start(SpiceServer *reds)
{
    GList *it;

    reds->vm_running = TRUE;
    for (it = reds->char_devices; it != NULL; it = it->next) {
        red_char_device_start(it->data);
    }
    reds_on_vm_start(reds);
}

SPICE_GNUC_VISIBLE void spice_server_vm_stop(SpiceServer *reds)
{
    GList *it;

    reds->vm_running = FALSE;
    for (it = reds->char_devices; it != NULL; it = it->next) {
        red_char_device_stop(it->data);
    }
    reds_on_vm_stop(reds);
}

SPICE_GNUC_VISIBLE void spice_server_set_seamless_migration(SpiceServer *reds, int enable)
{
    /* seamless migration is not supported with multiple clients */
    reds->seamless_migration_enabled = enable && !reds->allow_multiple_clients;
    spice_debug("seamless migration enabled=%d", enable);
}

GArray* reds_get_renderers(RedsState *reds)
{
    return reds->config->renderers;
}

spice_wan_compression_t reds_get_jpeg_state(const RedsState *reds)
{
    return reds->config->jpeg_state;
}

spice_wan_compression_t reds_get_zlib_glz_state(const RedsState *reds)
{
    return reds->config->zlib_glz_state;
}

SpiceCoreInterfaceInternal* reds_get_core_interface(RedsState *reds)
{
    return &reds->core;
}

SpiceWatch *reds_core_watch_add(RedsState *reds,
                                int fd, int event_mask,
                                SpiceWatchFunc func,
                                void *opaque)
{
   g_return_val_if_fail(reds != NULL, NULL);
   g_return_val_if_fail(reds->core.watch_add != NULL, NULL);

   return reds->core.watch_add(&reds->core, fd, event_mask, func, opaque);
}

void reds_core_watch_update_mask(RedsState *reds,
                                 SpiceWatch *watch,
                                 int event_mask)
{
   g_return_if_fail(reds != NULL);
   g_return_if_fail(reds->core.watch_update_mask != NULL);

   reds->core.watch_update_mask(&reds->core, watch, event_mask);
}

void reds_core_watch_remove(RedsState *reds, SpiceWatch *watch)
{
   g_return_if_fail(reds != NULL);
   g_return_if_fail(reds->core.watch_remove != NULL);

   reds->core.watch_remove(&reds->core, watch);
}

SpiceTimer *reds_core_timer_add(RedsState *reds,
                                SpiceTimerFunc func,
                                void *opaque)
{
   g_return_val_if_fail(reds != NULL, NULL);
   g_return_val_if_fail(reds->core.timer_add != NULL, NULL);

   return reds->core.timer_add(&reds->core, func, opaque);

}

void reds_core_timer_start(RedsState *reds,
                           SpiceTimer *timer,
                           uint32_t ms)
{
   g_return_if_fail(reds != NULL);
   g_return_if_fail(reds->core.timer_start != NULL);

   return reds->core.timer_start(&reds->core, timer, ms);
}

void reds_core_timer_cancel(RedsState *reds,
                            SpiceTimer *timer)
{
   g_return_if_fail(reds != NULL);
   g_return_if_fail(reds->core.timer_cancel != NULL);

   return reds->core.timer_cancel(&reds->core, timer);
}

void reds_core_timer_remove(RedsState *reds,
                            SpiceTimer *timer)
{
    if (timer == NULL) {
        return;
    }

    g_return_if_fail(reds != NULL);
    g_return_if_fail(reds->core.timer_remove != NULL);

    reds->core.timer_remove(&reds->core, timer);
}

void reds_update_client_mouse_allowed(RedsState *reds)
{
    int allow_now = FALSE;
    int x_res = 0;
    int y_res = 0;
    int num_active_workers = g_list_length(reds->qxl_instances);

    if (num_active_workers > 0) {
        QXLInstance *qxl;

        allow_now = TRUE;
        FOREACH_QXL_INSTANCE(reds, qxl) {
            if (red_qxl_get_allow_client_mouse(qxl, &x_res, &y_res, &allow_now)) {
                break;
            }
        }
    }

    if (allow_now || allow_now != reds->dispatcher_allows_client_mouse) {
        reds->monitor_mode.x_res = x_res;
        reds->monitor_mode.y_res = y_res;
        reds->dispatcher_allows_client_mouse = allow_now;
        reds_update_mouse_mode(reds);
        if (reds->is_client_mouse_allowed && inputs_channel_has_tablet(reds->inputs_channel)) {
            inputs_channel_set_tablet_logical_size(reds->inputs_channel, reds->monitor_mode.x_res, reds->monitor_mode.y_res);
        }
    }
}

static gboolean reds_use_client_monitors_config(RedsState *reds)
{
    QXLInstance *qxl;

    if (reds->qxl_instances == NULL) {
        return FALSE;
    }

    FOREACH_QXL_INSTANCE(reds, qxl) {
        if (!red_qxl_client_monitors_config(qxl, NULL))
            return FALSE;
    }
    return TRUE;
}

static void reds_client_monitors_config(RedsState *reds, VDAgentMonitorsConfig *monitors_config)
{
    QXLInstance *qxl;

    FOREACH_QXL_INSTANCE(reds, qxl) {
        if (!red_qxl_client_monitors_config(qxl, monitors_config)) {
            /* this is a normal condition, some qemu devices might not implement it */
            spice_debug("QXLInterface::client_monitors_config failed");
        }
    }
}

static int calc_compression_level(RedsState *reds)
{
    spice_assert(reds_get_streaming_video(reds) != SPICE_STREAM_VIDEO_INVALID);

    if ((reds_get_streaming_video(reds) != SPICE_STREAM_VIDEO_OFF) ||
        (spice_server_get_image_compression(reds) != SPICE_IMAGE_COMPRESSION_QUIC)) {
        return 0;
    } else {
        return 1;
    }
}

void reds_on_ic_change(RedsState *reds)
{
    int compression_level = calc_compression_level(reds);
    QXLInstance *qxl;

    FOREACH_QXL_INSTANCE(reds, qxl) {
        red_qxl_set_compression_level(qxl, compression_level);
        red_qxl_on_ic_change(qxl, spice_server_get_image_compression(reds));
    }
}

void reds_on_sv_change(RedsState *reds)
{
    int compression_level = calc_compression_level(reds);
    QXLInstance *qxl;

    FOREACH_QXL_INSTANCE(reds, qxl) {
        red_qxl_set_compression_level(qxl, compression_level);
        red_qxl_on_sv_change(qxl, reds_get_streaming_video(reds));
    }
}

void reds_on_vc_change(RedsState *reds)
{
    QXLInstance *qxl;

    FOREACH_QXL_INSTANCE(reds, qxl) {
        red_qxl_on_vc_change(qxl, reds_get_video_codecs(reds));
    }
}

void reds_on_vm_stop(RedsState *reds)
{
    QXLInstance *qxl;

    FOREACH_QXL_INSTANCE(reds, qxl) {
        red_qxl_stop(qxl);
    }
}

void reds_on_vm_start(RedsState *reds)
{
    QXLInstance *qxl;

    FOREACH_QXL_INSTANCE(reds, qxl) {
        red_qxl_start(qxl);
    }
}

uint32_t reds_qxl_ram_size(RedsState *reds)
{
    QXLInstance *first;
    if (!reds->qxl_instances) {
        return 0;
    }

    first = reds->qxl_instances->data;
    return red_qxl_get_ram_size(first);
}

MainDispatcher* reds_get_main_dispatcher(RedsState *reds)
{
    return reds->main_dispatcher;
}

static void red_char_device_vdi_port_constructed(GObject *object)
{
    RedCharDeviceVDIPort *dev = RED_CHAR_DEVICE_VDIPORT(object);
    RedsState *reds;

    G_OBJECT_CLASS(red_char_device_vdi_port_parent_class)->constructed(object);

    reds = red_char_device_get_server(RED_CHAR_DEVICE(object));

    agent_msg_filter_init(&dev->priv->write_filter, reds->config->agent_copypaste,
                          reds->config->agent_file_xfer,
                          reds_use_client_monitors_config(reds),
                          TRUE);
    agent_msg_filter_init(&dev->priv->read_filter, reds->config->agent_copypaste,
                          reds->config->agent_file_xfer,
                          reds_use_client_monitors_config(reds),
                          TRUE);
}

static void
red_char_device_vdi_port_init(RedCharDeviceVDIPort *self)
{
    self->priv = red_char_device_vdi_port_get_instance_private(self);

    self->priv->read_state = VDI_PORT_READ_STATE_READ_HEADER;
    self->priv->receive_pos = (uint8_t *)&self->priv->vdi_chunk_header;
    self->priv->receive_len = sizeof(self->priv->vdi_chunk_header);
}

static void
red_char_device_vdi_port_finalize(GObject *object)
{
    RedCharDeviceVDIPort *dev = RED_CHAR_DEVICE_VDIPORT(object);

    /* make sure we have no other references to RedVDIReadBuf buffers */
    red_char_device_reset(RED_CHAR_DEVICE(dev));
    if (dev->priv->current_read_buf) {
        red_pipe_item_unref(&dev->priv->current_read_buf->base);
        dev->priv->current_read_buf = NULL;
    }
    g_free(dev->priv->mig_data);
    if (spice_extra_checks) {
        spice_assert(dev->priv->num_read_buf == 0);
    }

    G_OBJECT_CLASS(red_char_device_vdi_port_parent_class)->finalize(object);
}

static void
red_char_device_vdi_port_class_init(RedCharDeviceVDIPortClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    RedCharDeviceClass *char_dev_class = RED_CHAR_DEVICE_CLASS(klass);

    object_class->finalize = red_char_device_vdi_port_finalize;
    object_class->constructed = red_char_device_vdi_port_constructed;

    char_dev_class->read_one_msg_from_device = vdi_port_read_one_msg_from_device;
    char_dev_class->send_msg_to_client = vdi_port_send_msg_to_client;
    char_dev_class->send_tokens_to_client = vdi_port_send_tokens_to_client;
    char_dev_class->remove_client = vdi_port_remove_client;
    char_dev_class->on_free_self_token = vdi_port_on_free_self_token;
}

static RedCharDeviceVDIPort *red_char_device_vdi_port_new(RedsState *reds)
{
    return g_object_new(RED_TYPE_CHAR_DEVICE_VDIPORT,
                        "spice-server", reds,
                        "client-tokens-interval", (guint64)REDS_TOKENS_TO_SEND,
                        "self-tokens", (guint64)REDS_NUM_INTERNAL_AGENT_MESSAGES,
                        NULL);
}

RedRecord *reds_get_record(RedsState *reds)
{
    if (reds->record) {
        return red_record_ref(reds->record);
    }

    return NULL;
}
