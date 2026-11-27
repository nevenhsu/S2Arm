// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#import "LauncherPadBridge.h"
#include "../../../../../macos/ns2_relay.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
// Reuse the existing Joy-Con orientation implementation, including its rumble
// dependency, rather than maintaining another interpretation of the raw bits.
#include "../../../../../runtime/ns2pro/NS2Rumble.c"
#include "../../../../../runtime/ns2pro/NS2JoyCon.c"

#define S2A_MOTION_QUEUE 512
struct s2a_launcher_pad_client {
    int fd;
    size_t used;
    unsigned char bytes[sizeof(ns2_relay_packet)];
    int motion;   // 面板开了体感：才排六轴
    unsigned head, count;
    s2a_launcher_pad_motion queue[S2A_MOTION_QUEUE];
};

int s2a_launcher_pad_decode_motion(const void *bytes, size_t length, s2a_launcher_pad_motion out[2]) {
    if (!bytes || !out || length != sizeof(ns2_relay_packet)) return -1;
    ns2_relay_packet p;
    memcpy(&p, bytes, sizeof p);
    if (p.magic != NS2_RELAY_MAGIC || p.version != NS2_RELAY_VERSION || p.type != NS2_RELAY_INPUT) return -1;
    int n = 0;
    if (!p.ready) return 0;
    if (p.motion_known && p.motion.known)
        out[n++] = (s2a_launcher_pad_motion){p.player, p.kind, 0, p.motion.host_timestamp_us,
            p.motion.acceleration_x, p.motion.acceleration_y, p.motion.acceleration_z, p.motion.gyro_x, p.motion.gyro_y, p.motion.gyro_z};
    if (p.motion_left_known) {
        const ns2_relay_motion_left *l = &p.motion_left;
        out[n++] = (s2a_launcher_pad_motion){p.player, p.kind, 1, (uint64_t)l->host_timestamp_us_hi << 32 | l->host_timestamp_us_lo,
            l->acceleration_x, l->acceleration_y, l->acceleration_z, l->gyro_x, l->gyro_y, l->gyro_z};
    }
    return n;
}

size_t s2a_launcher_pad_encode_motion_for_test(const s2a_launcher_pad_motion *m, int n, void *out, size_t capacity) {
    if (!m || n < 1 || !out || capacity < sizeof(ns2_relay_packet)) return 0;
    ns2_relay_packet p;
    memset(&p, 0, sizeof p);
    p.magic = NS2_RELAY_MAGIC; p.version = NS2_RELAY_VERSION; p.type = NS2_RELAY_INPUT; p.ready = 1;
    p.player = m[0].player; p.kind = n == 2 ? NS2_KIND_JOYCON_PAIR : m[0].kind;
    p.motion_known = 1; p.motion.known = 1; p.motion.host_timestamp_us = m[0].host_timestamp_us;
    p.motion.acceleration_x = m[0].ax; p.motion.acceleration_y = m[0].ay; p.motion.acceleration_z = m[0].az;
    p.motion.gyro_x = m[0].gx; p.motion.gyro_y = m[0].gy; p.motion.gyro_z = m[0].gz;
    if (n == 2) {
        p.motion_left_known = 1;
        p.motion_left = (ns2_relay_motion_left){m[1].ax, m[1].ay, m[1].az, m[1].gx, m[1].gy, m[1].gz,
            (uint32_t)m[1].host_timestamp_us, (uint32_t)(m[1].host_timestamp_us >> 32)};
    }
    memcpy(out, &p, sizeof p);
    return sizeof p;
}

static void queue_motion(s2a_launcher_pad_client *c, const unsigned char *bytes) {
    s2a_launcher_pad_motion m[2];
    int n = s2a_launcher_pad_decode_motion(bytes, sizeof(ns2_relay_packet), m);
    for (int i = 0; i < n; i++) {
        if (c->count == S2A_MOTION_QUEUE) { c->head = (c->head + 1) % S2A_MOTION_QUEUE; c->count--; }   // 面板没来取：丢最旧的
        c->queue[(c->head + c->count) % S2A_MOTION_QUEUE] = m[i];
        c->count++;
    }
}

int s2a_launcher_pad_set_motion(s2a_launcher_pad_client *c, int enabled) {
    if (!c) return -1;
    ns2_relay_packet p;
    memset(&p, 0, sizeof p);
    p.magic = NS2_RELAY_MAGIC; p.version = NS2_RELAY_VERSION; p.type = NS2_RELAY_MOTION; p.enabled = enabled ? 1 : 0;
    ssize_t n = send(c->fd, &p, sizeof p, MSG_DONTWAIT);
    if (n != (ssize_t)sizeof p) return -1;
    c->motion = enabled ? 1 : 0;
    if (!enabled) c->head = c->count = 0;
    return 0;
}

int s2a_launcher_pad_motion_drain(s2a_launcher_pad_client *c, s2a_launcher_pad_motion *out, int max) {
    if (!c || !out || max <= 0) return 0;
    int n = 0;
    while (c->count && n < max) { out[n++] = c->queue[c->head]; c->head = (c->head + 1) % S2A_MOTION_QUEUE; c->count--; }
    return n;
}

int s2a_launcher_pad_decode(const void *bytes, size_t length, s2a_launcher_pad_sample *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!bytes || length != sizeof(ns2_relay_packet)) return -1;
    ns2_relay_packet p;
    memcpy(&p, bytes, sizeof p);
    if (p.magic != NS2_RELAY_MAGIC || p.version != NS2_RELAY_VERSION || p.type != NS2_RELAY_INPUT) return -1;
    if (p.player != 0) return 0; // The launcher's navigation belongs to player 1.
    out->epoch = p.epoch;
    if (!p.ready) return 1;
    ns2_joycon_raw_to_sideways((ns2_kind)p.kind, &p.state);
    out->ready = 1;
    out->buttons = p.state.buttons;
    out->x = p.state.lx / 32768.0f;
    out->y = p.state.ly / 32768.0f;
    out->right_y = p.state.ry / 32768.0f;
    return 1;
}

s2a_launcher_pad_client *s2a_launcher_pad_open(const char *path) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (!path || strlen(path) >= sizeof addr.sun_path) return NULL;
    strlcpy(addr.sun_path, path, sizeof addr.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);   // 面板送体感开关时连接已断：报错，不让 SIGPIPE 结束启动器
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 && errno != EINPROGRESS)) { close(fd); return NULL; }
    s2a_launcher_pad_client *c = calloc(1, sizeof *c);
    if (!c) { close(fd); return NULL; }
    c->fd = fd;
    return c;
}

int s2a_launcher_pad_poll(s2a_launcher_pad_client *c, s2a_launcher_pad_sample *sample) {
    if (!c || !sample) return -1;
    int found = 0;
    for (unsigned count = 0; count < 32; count++) {
        ssize_t n = read(c->fd, c->bytes + c->used, sizeof c->bytes - c->used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return found;
        if (n <= 0) return -1;
        c->used += (size_t)n;
        if (c->used == sizeof c->bytes) {
            if (c->motion) queue_motion(c, c->bytes);
            s2a_launcher_pad_sample next;
            int result = s2a_launcher_pad_decode(c->bytes, c->used, &next);
            c->used = 0;
            if (result < 0) return -1;
            if (result > 0) { *sample = next; found = 1; }
        }
    }
    return found;
}
void s2a_launcher_pad_close(s2a_launcher_pad_client *c) {
    if (c) { shutdown(c->fd, SHUT_RDWR); close(c->fd); free(c); }
}
