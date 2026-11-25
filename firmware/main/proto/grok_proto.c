#include "grok_proto.h"

#include <string.h>

static int read_exact(grok_io_read_fn read_fn, void *ctx, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = read_fn(ctx, buf + got, len - got);
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static int write_exact(grok_io_write_fn write_fn, void *ctx, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int w = write_fn(ctx, buf + sent, len - sent);
        if (w <= 0) {
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

void grok_proto_pack_header(uint8_t out[GROK_PROTO_HEADER_LEN], uint8_t msg_type, uint8_t flags,
                            uint16_t len)
{
    out[0] = GROK_PROTO_MAGIC_0;
    out[1] = GROK_PROTO_MAGIC_1;
    out[2] = msg_type;
    out[3] = flags;
    out[4] = (uint8_t)(len & 0xFF);
    out[5] = (uint8_t)((len >> 8) & 0xFF);
}

int grok_proto_write_frame(grok_io_write_fn write_fn, void *ctx, uint8_t msg_type, uint8_t flags,
                           const uint8_t *payload, uint16_t len)
{
    if (!write_fn || len > GROK_PROTO_MAX_PAYLOAD) {
        return -1;
    }
    uint8_t hdr[GROK_PROTO_HEADER_LEN];
    grok_proto_pack_header(hdr, msg_type, flags, len);

    if (write_exact(write_fn, ctx, hdr, sizeof(hdr)) != 0) {
        return -1;
    }
    if (len > 0 && payload) {
        if (write_exact(write_fn, ctx, payload, len) != 0) {
            return -1;
        }
    }
    return 0;
}

int grok_proto_read_frame(grok_io_read_fn read_fn, void *ctx, grok_frame_hdr_t *hdr_out,
                          uint8_t *payload_buf, size_t payload_cap)
{
    if (!read_fn || !hdr_out) {
        return -1;
    }
    uint8_t hdr[GROK_PROTO_HEADER_LEN];
    if (read_exact(read_fn, ctx, hdr, sizeof(hdr)) != 0) {
        return -1;
    }
    if (hdr[0] != GROK_PROTO_MAGIC_0 || hdr[1] != GROK_PROTO_MAGIC_1) {
        return -1;
    }
    uint16_t len = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
    if (len > GROK_PROTO_MAX_PAYLOAD || len > payload_cap) {
        return -1;
    }

    hdr_out->msg_type = hdr[2];
    hdr_out->flags = hdr[3];
    hdr_out->len = len;

    if (len > 0) {
        if (!payload_buf || read_exact(read_fn, ctx, payload_buf, len) != 0) {
            return -1;
        }
    }
    return 0;
}
