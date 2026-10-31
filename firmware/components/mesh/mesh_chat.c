#include "mesh_chat.h"

#include "channel_input.h"
#include "mesh_frame.h"
#include "mesh_outbox.h"

#include <stdio.h>
#include <string.h>

mesh_status_kind_t mesh_msg_status(mesh_msg_t const *m, char *out, size_t cap) {
    if (cap) {
        out[0] = '\0';
    }
    if (!m->outgoing) {
        return MESH_STATUS_NONE;
    }
    switch ((mesh_out_state_t)m->state) {
    case MESH_OUT_QUEUED:
    case MESH_OUT_SENT:
        snprintf(out, cap, "queued");
        return MESH_STATUS_PENDING;
    case MESH_OUT_ON_AIR:
        snprintf(out, cap, "on air");
        return MESH_STATUS_PENDING;
    case MESH_OUT_HEARD:
        if (m->repeats > 1) {
            snprintf(out, cap, "heard repeated x%u", (unsigned)m->repeats);
        } else {
            snprintf(out, cap, "heard repeated");
        }
        return MESH_STATUS_OK;
    case MESH_OUT_DELIVERED:
        snprintf(out, cap, "delivered");
        return MESH_STATUS_OK;
    case MESH_OUT_FAILED:
    default:
        snprintf(out, cap, "not sent");
        return MESH_STATUS_FAILED;
    }
}

bool mesh_msg_in_channel(mesh_msg_t const *m, char const *channel) {
    return channel_same_name(m->channel, channel);
}

size_t mesh_compose_status(char const *text, size_t max_bytes, char *out, size_t cap) {
    size_t len  = strlen(text);
    size_t sent = mf_text_cut(text, max_bytes);
    if (sent < len) {
        snprintf(out, cap, "%u / %u bytes, the last %u are cut", (unsigned)len, (unsigned)max_bytes,
                 (unsigned)(len - sent));
    } else {
        snprintf(out, cap, "%u / %u bytes", (unsigned)len, (unsigned)max_bytes);
    }
    return sent;
}
