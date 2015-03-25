/****************************************************************
 *
 *        Copyright 2015, Big Switch Networks, Inc.
 *
 * Licensed under the Eclipse Public License, Version 1.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *        http://www.eclipse.org/legal/epl-v10.html
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the
 * License.
 *
 ****************************************************************/

#include <packet_trace/packet_trace.h>
#include <AIM/aim.h>
#include <AIM/aim_list.h>
#include <linux/un.h>
#include <unistd.h>
#include <sys/socket.h>
#include <SocketManager/socketmanager.h>
#include <errno.h>

#define AIM_LOG_MODULE_NAME packet_trace
#include <AIM/aim_log.h>

AIM_LOG_STRUCT_DEFINE(AIM_LOG_OPTIONS_DEFAULT, AIM_LOG_BITS_DEFAULT, NULL, 0);

#define MAX_PORTS 1024
#define READ_BUFFER_SIZE 1024
#define LISTEN_BACKLOG 5

struct client {
    struct list_links links;
    int fd;
    aim_bitmap_t ports;
    int read_buffer_offset;
    char read_buffer[READ_BUFFER_SIZE];
};

struct packet {
    uint32_t in_port;
};

static bool check_subscribed(struct client *client);
static void listen_callback(int socket_id, void *cookie, int read_ready, int write_ready, int error_seen);
static void client_callback(int socket_id, void *cookie, int read_ready, int write_ready, int error_seen);
static void destroy_client(struct client *client);
static void process_command(struct client *client, char *command);

bool packet_trace_enabled;
static LIST_DEFINE(clients);
static aim_pvs_t *pvs;
static struct packet packet;
static int listen_socket;

void
packet_trace_init(const char *name)
{
    pvs = aim_pvs_buffer_create();

    char path[UNIX_PATH_MAX];
    snprintf(path, sizeof(path), "/var/run/ivs-packet-trace.%s.sock", name);

    unlink(path);

    listen_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_socket < 0) {
        perror("socket (packet_trace)");
        abort();
    }

    struct sockaddr_un saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sun_family = AF_UNIX;
    strcpy(saddr.sun_path, path);

    if (bind(listen_socket, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("bind (packet_trace)");
        abort();
    }

    if (listen(listen_socket, LISTEN_BACKLOG) < 0) {
        perror("listen (packet_trace)");
        abort();
    }

    indigo_error_t rv = ind_soc_socket_register(listen_socket, listen_callback, NULL);
    if (rv < 0) {
        AIM_DIE("Failed to register packet_trace socket: %s", indigo_strerror(rv));
    }
}

void
packet_trace_begin(uint32_t in_port)
{
    packet.in_port = in_port;

    packet_trace_enabled = false;

    list_links_t *cur;
    LIST_FOREACH(&clients, cur) {
        struct client *client = container_of(cur, links, struct client);
        if (check_subscribed(client)) {
            packet_trace_enabled = true;
            break;
        }
    }

    packet_trace("--------------------------------------------------------------");
}

void
packet_trace_end(void)
{
    if (!packet_trace_enabled) {
        return;
    }

    char *buf = aim_pvs_buffer_get(pvs);
    int len = aim_pvs_buffer_size(pvs);
    aim_pvs_buffer_reset(pvs);

    list_links_t *cur;
    LIST_FOREACH(&clients, cur) {
        struct client *client = container_of(cur, links, struct client);
        if (!check_subscribed(client)) {
            continue;
        }
        AIM_LOG_TRACE("writing to client %d (%d bytes)", client->fd, len);
        int written = 0;
        while (written < len) {
            int c = write(client->fd, buf+written, len-written);
            if (c < 0) {
                break;
            } else if (c == 0) {
                break;
            } else {
                written += c;
            }
        }
    }

    aim_free(buf);
}

void
packet_trace_internal(const char *fmt, va_list vargs)
{
    aim_vprintf(pvs, fmt, vargs);
    aim_printf(pvs, "\n");
}

void
packet_trace_set_fd_bitmap(aim_bitmap_t *bitmap)
{
    list_links_t *cur;
    LIST_FOREACH(&clients, cur) {
        struct client *client = container_of(cur, links, struct client);
        AIM_BITMAP_SET(bitmap, client->fd);
    }
}

static bool
check_subscribed(struct client *client)
{
    return AIM_BITMAP_GET(&client->ports, packet.in_port);
}

static void
listen_callback(
    int socket_id,
    void *cookie,
    int read_ready,
    int write_ready,
    int error_seen)
{
    AIM_LOG_TRACE("Accepting packet_trace client");

    int fd;
    if ((fd = accept(listen_socket, NULL, NULL)) < 0) {
        AIM_LOG_ERROR("Failed to accept on packet_trace socket: %s", strerror(errno));
        return;
    }

    struct client *client = aim_zmalloc(sizeof(*client));
    list_push(&clients, &client->links);
    client->fd = fd;
    aim_bitmap_alloc(&client->ports, MAX_PORTS);

    indigo_error_t rv = ind_soc_socket_register(fd, client_callback, client);
    if (rv < 0) {
        AIM_LOG_ERROR("Failed to register packet_trace client socket: %s", indigo_strerror(rv));
        return;
    }
}

static void
client_callback(
    int socket_id,
    void *cookie,
    int read_ready,
    int write_ready,
    int error_seen)
{
    struct client *client = cookie;
    AIM_ASSERT(socket_id == client->fd);

    if (error_seen) {
        int socket_error = 0;
        socklen_t len = sizeof(socket_error);
        getsockopt(socket_id, SOL_SOCKET, SO_ERROR, &socket_error, &len);
        AIM_LOG_TRACE("Error seen on packet_trace socket: %s", strerror(socket_error));
        destroy_client(client);
        return;
    }

    if (read_ready) {
        int c;
        if ((c = read(client->fd, client->read_buffer+client->read_buffer_offset,
                      READ_BUFFER_SIZE - client->read_buffer_offset)) < 0) {
            AIM_LOG_ERROR("read failed: %s", strerror(errno));
            return;
        }

        client->read_buffer_offset += c;

        if (c == 0) {
            /* Peer has shutdown their write side */
            destroy_client(client);
            return;
        }

        /* Process each complete line */
        char *newline;
        char *start = client->read_buffer;
        int remaining = client->read_buffer_offset;
        while ((newline = memchr(start, '\n', remaining))) {
            *newline = '\0';
            process_command(client, start);
            remaining -= newline - start + 1;
            start = newline + 1;
        }

        /* Move incomplete line (which may be empty) to the beginning of the read buffer */
        if (client->read_buffer != start) {
            memmove(client->read_buffer, start, remaining);
            client->read_buffer_offset = remaining;
        } else if (client->read_buffer_offset == READ_BUFFER_SIZE) {
            AIM_LOG_WARN("Disconnecting packet_trace client due to too-long line");
            destroy_client(client);
            return;
        }
    }
}

static void
destroy_client(struct client *client)
{
    ind_soc_socket_unregister(client->fd);
    close(client->fd);
    list_remove(&client->links);
    aim_free(client);
}

static void
reply(struct client *client, const char *fmt, ...)
{
    va_list vargs;
    va_start(vargs, fmt);
    dprintf(client->fd, fmt, vargs);
    va_end(vargs);
}

static void
process_add_command(struct client *client, const char **argv, int argc)
{
    if (!strcmp(argv[0], "port")) {
        if (argc != 2) {
            reply(client, "expected 2 arguments\n");
            return;
        }
        uint32_t port = atoi(argv[1]);
        if (port >= MAX_PORTS) {
            reply(client, "invalid port number\n");
        } else {
            AIM_BITMAP_SET(&client->ports, port);
        }
    } else if (!strcmp(argv[0], "all")) {
        if (argc != 1) {
            reply(client, "expected 1 argument\n");
            return;
        }
        AIM_BITMAP_SET_ALL(&client->ports);
    } else {
        reply(client, "unexpected filter type\n");
    }
}

static void
process_command(struct client *client, char *command)
{
    aim_tokens_t *tokens = aim_strsplit(command, " ");

    if (tokens->count < 1) {
        /* ignore empty line */
    } else {
        const char *cmd = tokens->tokens[0];
        if (!strcmp(cmd, "add")) {
            process_add_command(client, tokens->tokens+1, tokens->count-1);
        } else {
            reply(client, "unknown command\n");
        }
    }

    aim_tokens_free(tokens);
}
