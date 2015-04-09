/*
   Copyright (C) 2009-2016 Red Hat, Inc.

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

#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>

#define SPICE_LOG_DOMAIN "SpiceDispatcher"

#include <common/mem.h>
#include <common/spice_common.h>
#include "dispatcher.h"

//#define DEBUG_DISPATCHER

#ifdef DEBUG_DISPATCHER
#include <signal.h>

static void setup_dummy_signal_handler(void);
#endif

G_DEFINE_TYPE(Dispatcher, dispatcher, G_TYPE_OBJECT)

#define DISPATCHER_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TYPE_DISPATCHER, DispatcherPrivate))

struct DispatcherPrivate {
    int recv_fd;
    int send_fd;
    pthread_t thread_id;
    pthread_mutex_t lock;
    DispatcherMessage *messages;
    int stage;  /* message parser stage - sender has no stages */
    guint max_message_type;
    void *payload; /* allocated as max of message sizes */
    size_t payload_size; /* used to track realloc calls */
    void *opaque;
    dispatcher_handle_async_done handle_async_done;
    dispatcher_handle_any_message any_handler;
};

enum {
    PROP_0,
    PROP_MAX_MESSAGE_TYPE,
    PROP_OPAQUE
};

static void
dispatcher_get_property(GObject    *object,
                        guint       property_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
    Dispatcher *self = DISPATCHER(object);

    switch (property_id)
    {
        case PROP_MAX_MESSAGE_TYPE:
            g_value_set_uint(value, self->priv->max_message_type);
            break;
        case PROP_OPAQUE:
            g_value_set_pointer(value, self->priv->opaque);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
dispatcher_set_property(GObject      *object,
                        guint         property_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
    Dispatcher *self = DISPATCHER(object);

    switch (property_id)
    {
        case PROP_MAX_MESSAGE_TYPE:
            self->priv->max_message_type = g_value_get_uint(value);
            break;
        case PROP_OPAQUE:
            dispatcher_set_opaque(self, g_value_get_pointer(value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
dispatcher_finalize(GObject *object)
{
    Dispatcher *self = DISPATCHER(object);
    g_free(self->priv->messages);
    close(self->priv->send_fd);
    close(self->priv->recv_fd);
    pthread_mutex_destroy(&self->priv->lock);
    free(self->priv->payload);
    G_OBJECT_CLASS(dispatcher_parent_class)->finalize(object);
}

static void dispatcher_constructed(GObject *object)
{
    Dispatcher *self = DISPATCHER(object);
    int channels[2];

    G_OBJECT_CLASS(dispatcher_parent_class)->constructed(object);

#ifdef DEBUG_DISPATCHER
    setup_dummy_signal_handler();
#endif
    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, channels) == -1) {
        spice_error("socketpair failed %s", strerror(errno));
        return;
    }
    pthread_mutex_init(&self->priv->lock, NULL);
    self->priv->recv_fd = channels[0];
    self->priv->send_fd = channels[1];
    self->priv->thread_id = pthread_self();

    self->priv->messages = g_new0(DispatcherMessage,
                                  self->priv->max_message_type);
}

static void
dispatcher_class_init(DispatcherClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    g_type_class_add_private(klass, sizeof (DispatcherPrivate));

    object_class->get_property = dispatcher_get_property;
    object_class->set_property = dispatcher_set_property;
    object_class->constructed = dispatcher_constructed;
    object_class->finalize = dispatcher_finalize;

    g_object_class_install_property(object_class,
                                    PROP_MAX_MESSAGE_TYPE,
                                    g_param_spec_uint("max-message-type",
                                                      "max-message-type",
                                                      "Maximum message type",
                                                      0, G_MAXUINT, 0,
                                                      G_PARAM_STATIC_STRINGS |
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT_ONLY));
    g_object_class_install_property(object_class,
                                    PROP_OPAQUE,
                                    g_param_spec_pointer("opaque",
                                                         "opaque",
                                                         "User data to pass to callbacks",
                                                         G_PARAM_STATIC_STRINGS |
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT));

}

static void
dispatcher_init(Dispatcher *self)
{
    self->priv = DISPATCHER_PRIVATE(self);
}

Dispatcher *
dispatcher_new(size_t max_message_type, void *opaque)
{
    return g_object_new(TYPE_DISPATCHER,
                        "max-message-type", (guint) max_message_type,
                        "opaque", opaque,
                        NULL);
}


#define ACK 0xffffffff

/*
 * read_safe
 * helper. reads until size bytes accumulated in buf, if an error other than
 * EINTR is encountered returns -1, otherwise returns 0.
 * @block if 1 the read will block (the fd is always blocking).
 *        if 0 poll first, return immediately if no bytes available, otherwise
 *         read size in blocking mode.
 */
static int read_safe(int fd, uint8_t *buf, size_t size, int block)
{
    int read_size = 0;
    int ret;
    struct pollfd pollfd = {.fd = fd, .events = POLLIN, .revents = 0};

    if (size == 0) {
        return 0;
    }

    if (!block) {
        while ((ret = poll(&pollfd, 1, 0)) == -1) {
            if (errno == EINTR) {
                spice_debug("EINTR in poll");
                continue;
            }
            spice_error("poll failed");
            return -1;
        }
        if (!(pollfd.revents & POLLIN)) {
            return 0;
        }
    }
    while (read_size < size) {
        ret = read(fd, buf + read_size, size - read_size);
        if (ret == -1) {
            if (errno == EINTR) {
                spice_debug("EINTR in read");
                continue;
            }
            return -1;
        }
        if (ret == 0) {
            spice_error("broken pipe on read");
            return -1;
        }
        read_size += ret;
    }
    return read_size;
}

/*
 * write_safe
 * @return -1 for error, otherwise number of written bytes. may be zero.
 */
static int write_safe(int fd, uint8_t *buf, size_t size)
{
    int written_size = 0;
    int ret;

    while (written_size < size) {
        ret = write(fd, buf + written_size, size - written_size);
        if (ret == -1) {
            if (errno != EINTR) {
                return -1;
            }
            spice_debug("EINTR in write");
            continue;
        }
        written_size += ret;
    }
    return written_size;
}

static int dispatcher_handle_single_read(Dispatcher *dispatcher)
{
    int ret;
    uint32_t type;
    DispatcherMessage *msg = NULL;
    uint8_t *payload = dispatcher->priv->payload;
    uint32_t ack = ACK;

    if ((ret = read_safe(dispatcher->priv->recv_fd, (uint8_t*)&type, sizeof(type), 0)) == -1) {
        spice_printerr("error reading from dispatcher: %d", errno);
        return 0;
    }
    if (ret == 0) {
        /* no messsage */
        return 0;
    }
    msg = &dispatcher->priv->messages[type];
    if (read_safe(dispatcher->priv->recv_fd, payload, msg->size, 1) == -1) {
        spice_printerr("error reading from dispatcher: %d", errno);
        /* TODO: close socketpair? */
        return 0;
    }
    if (dispatcher->priv->any_handler) {
        dispatcher->priv->any_handler(dispatcher->priv->opaque, type, payload);
    }
    if (msg->handler) {
        msg->handler(dispatcher->priv->opaque, payload);
    } else {
        spice_printerr("error: no handler for message type %d", type);
    }
    if (msg->ack == DISPATCHER_ACK) {
        if (write_safe(dispatcher->priv->recv_fd,
                       (uint8_t*)&ack, sizeof(ack)) == -1) {
            spice_printerr("error writing ack for message %d", type);
            /* TODO: close socketpair? */
        }
    } else if (msg->ack == DISPATCHER_ASYNC && dispatcher->priv->handle_async_done) {
        dispatcher->priv->handle_async_done(dispatcher->priv->opaque, type, payload);
    }
    return 1;
}

/*
 * dispatcher_handle_recv_read
 * doesn't handle being in the middle of a message. all reads are blocking.
 */
void dispatcher_handle_recv_read(Dispatcher *dispatcher)
{
    while (dispatcher_handle_single_read(dispatcher)) {
    }
}

void dispatcher_send_message(Dispatcher *dispatcher, uint32_t message_type,
                             void *payload)
{
    DispatcherMessage *msg;
    uint32_t ack;
    int send_fd = dispatcher->priv->send_fd;

    assert(dispatcher->priv->max_message_type > message_type);
    assert(dispatcher->priv->messages[message_type].handler);
    msg = &dispatcher->priv->messages[message_type];
    pthread_mutex_lock(&dispatcher->priv->lock);
    if (write_safe(send_fd, (uint8_t*)&message_type, sizeof(message_type)) == -1) {
        spice_printerr("error: failed to send message type for message %d",
                   message_type);
        goto unlock;
    }
    if (write_safe(send_fd, payload, msg->size) == -1) {
        spice_printerr("error: failed to send message body for message %d",
                   message_type);
        goto unlock;
    }
    if (msg->ack == DISPATCHER_ACK) {
        if (read_safe(send_fd, (uint8_t*)&ack, sizeof(ack), 1) == -1) {
            spice_printerr("error: failed to read ack");
        } else if (ack != ACK) {
            spice_printerr("error: got wrong ack value in dispatcher "
                       "for message %d\n", message_type);
            /* TODO handling error? */
        }
    }
unlock:
    pthread_mutex_unlock(&dispatcher->priv->lock);
}

void dispatcher_register_async_done_callback(
                                        Dispatcher *dispatcher,
                                        dispatcher_handle_async_done handler)
{
    assert(dispatcher->priv->handle_async_done == NULL);
    dispatcher->priv->handle_async_done = handler;
}

void dispatcher_register_handler(Dispatcher *dispatcher, uint32_t message_type,
                                 dispatcher_handle_message handler,
                                 size_t size, int ack)
{
    DispatcherMessage *msg;

    assert(message_type < dispatcher->priv->max_message_type);
    assert(dispatcher->priv->messages[message_type].handler == 0);
    msg = &dispatcher->priv->messages[message_type];
    msg->handler = handler;
    msg->size = size;
    msg->ack = ack;
    if (msg->size > dispatcher->priv->payload_size) {
        dispatcher->priv->payload = realloc(dispatcher->priv->payload, msg->size);
        dispatcher->priv->payload_size = msg->size;
    }
}

void dispatcher_register_universal_handler(
                               Dispatcher *dispatcher,
                               dispatcher_handle_any_message any_handler)
{
    dispatcher->priv->any_handler = any_handler;
}

#ifdef DEBUG_DISPATCHER
static void dummy_handler(int bla)
{
}

static void setup_dummy_signal_handler(void)
{
    static int inited = 0;
    struct sigaction act = {
        .sa_handler = &dummy_handler,
    };
    if (inited) {
        return;
    }
    inited = 1;
    /* handle SIGRTMIN+10 in order to test the loops for EINTR */
    if (sigaction(SIGRTMIN + 10, &act, NULL) == -1) {
        fprintf(stderr,
            "failed to set dummy sigaction for DEBUG_DISPATCHER\n");
        exit(1);
    }
}
#endif

void dispatcher_set_opaque(Dispatcher *self, void *opaque)
{
    self->priv->opaque = opaque;
    g_object_notify(G_OBJECT(self), "opaque");
}

int dispatcher_get_recv_fd(Dispatcher *dispatcher)
{
    return dispatcher->priv->recv_fd;
}

pthread_t dispatcher_get_thread_id(Dispatcher *self)
{
    return self->priv->thread_id;
}
