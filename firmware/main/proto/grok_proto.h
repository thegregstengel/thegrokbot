#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GROK_PROTO_MAGIC_0      0x47 /* 'G' */
#define GROK_PROTO_MAGIC_1      0x42 /* 'B' */
#define GROK_PROTO_HEADER_LEN   6
#define GROK_PROTO_MAX_PAYLOAD  4096
#define GROK_PROTO_VERSION      1
#define GROK_AUTH_NONCE_LEN     16
#define GROK_AUTH_HMAC_LEN      32

/* Body -> Brain */
#define GROK_MSG_HELLO          0x01
#define GROK_MSG_AUDIO_UP       0x02
#define GROK_MSG_WAKE           0x03
#define GROK_MSG_BARGE_IN       0x04
#define GROK_MSG_SLEEP          0x05
#define GROK_MSG_PING           0x06
#define GROK_MSG_AUTH_RESPONSE  0x07

/* Brain -> Body */
#define GROK_MSG_WELCOME        0x81
#define GROK_MSG_AUDIO_DOWN     0x82
#define GROK_MSG_FACE_STATE     0x83
#define GROK_MSG_STOP_AUDIO     0x84
#define GROK_MSG_CONFIG         0x85
#define GROK_MSG_PONG           0x86
#define GROK_MSG_AUTH_CHALLENGE 0x87

#define GROK_WAKE_SRC_WAKEWORD  1
#define GROK_WAKE_SRC_TOUCH     2

#define GROK_CAP_AEC            (1 << 0)
#define GROK_CAP_WAKEWORD       (1 << 1)

typedef int (*grok_io_write_fn)(void *ctx, const uint8_t *buf, size_t len);
typedef int (*grok_io_read_fn)(void *ctx, uint8_t *buf, size_t len);

typedef struct {
    uint8_t msg_type;
    uint8_t flags;
    uint16_t len;
} grok_frame_hdr_t;

void grok_proto_pack_header(uint8_t out[GROK_PROTO_HEADER_LEN], uint8_t msg_type, uint8_t flags,
                            uint16_t len);

/* Returns 0 on success, -1 on I/O or protocol error. */
int grok_proto_write_frame(grok_io_write_fn write_fn, void *ctx, uint8_t msg_type, uint8_t flags,
                           const uint8_t *payload, uint16_t len);

/* Reads one complete frame into payload_buf (up to payload_cap). Returns 0 on success, -1 on error. */
int grok_proto_read_frame(grok_io_read_fn read_fn, void *ctx, grok_frame_hdr_t *hdr_out,
                          uint8_t *payload_buf, size_t payload_cap);

#ifdef __cplusplus
}
#endif
