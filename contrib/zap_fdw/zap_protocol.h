/*
 * zap_protocol.h — canonical ZAP-HTTP wire codec for hanzo/sql.
 *
 * Implements the transport and frame codec of github.com/zap-proto/http
 * (transport.go, codec.go, wire.go), the one internal transport the ORM,
 * gateway, and ingress speak. hanzo/sql answers it natively so a client using
 * hanzoai/orm's ZAP driver talks to Postgres with no sidecar.
 *
 * Transport: each message is one length-prefixed frame — a 4-byte BIG-ENDIAN
 * length N followed by N frame bytes.
 *
 * Frame: a zap-proto/go message — a 16-byte header then a root object.
 *   header: [0:4] "ZAP\0"  [4:6] version u16 LE  [6:8] flags u16 LE
 *           [8:12] rootOffset u32 LE (=16)  [12:16] size u32 LE (= frame length)
 *   type = flags >> 8 (request=1, response=2).
 *   root object: a fixed 48-byte section of 8-byte {relOffset u32 LE, length u32
 *   LE} slots; a non-empty field's bytes sit in the variable tail at
 *   slot+relOffset, an empty field is a zeroed {0,0} slot.
 */
#ifndef ZAP_PROTOCOL_H
#define ZAP_PROTOCOL_H

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#define ZAP_MAGIC        "ZAP\0"
#define ZAP_HEADER_SIZE  16
#define ZAP_ROOT_OFFSET  16

/* frame type (flags >> 8) */
#define ZAP_FRAME_REQUEST  1
#define ZAP_FRAME_RESPONSE 2

/* request root object slots */
#define ZAP_REQ_METHOD   0
#define ZAP_REQ_TARGET   8
#define ZAP_REQ_PROTO    16
#define ZAP_REQ_HEADERS  24
#define ZAP_REQ_BODY     32
#define ZAP_REQ_TRAILER  40
#define ZAP_REQ_SLOTSIZE 48

/* response root object slots */
#define ZAP_RESP_STATUS   0   /* u16 scalar */
#define ZAP_RESP_REASON   8
#define ZAP_RESP_PROTO    16
#define ZAP_RESP_HEADERS  24
#define ZAP_RESP_BODY     32
#define ZAP_RESP_TRAILER  40
#define ZAP_RESP_SLOTSIZE 48

/* little-endian scalars (frame body) */
static inline uint16_t zap_rd_u16(const uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}
static inline uint32_t zap_rd_u32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static inline void zap_wr_u16(uint8_t *b, uint16_t v) {
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF;
}
static inline void zap_wr_u32(uint8_t *b, uint32_t v) {
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF;
    b[2] = (v >> 16) & 0xFF; b[3] = (v >> 24) & 0xFF;
}

/* big-endian length prefix (transport) */
static inline uint32_t zap_rd_u32be(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}
static inline void zap_wr_u32be(uint8_t *b, uint32_t v) {
    b[0] = (v >> 24) & 0xFF; b[1] = (v >> 16) & 0xFF;
    b[2] = (v >> 8) & 0xFF; b[3] = v & 0xFF;
}

/*
 * Validate a frame's header and return the root object offset and frame size.
 * Returns 0 on success, -1 if the frame is malformed or not want_type.
 */
static inline int zap_frame_root(const uint8_t *f, uint32_t flen, int want_type,
                                 uint32_t *root_out, uint32_t *size_out) {
    uint16_t ver, type;
    uint32_t size, root;

    if (flen < ZAP_HEADER_SIZE)
        return -1;
    if (memcmp(f, ZAP_MAGIC, 4) != 0)
        return -1;
    ver = zap_rd_u16(f + 4);
    if (ver != 1 && ver != 2)
        return -1;
    size = zap_rd_u32(f + 12);
    if (size < ZAP_HEADER_SIZE || size > flen)
        return -1;
    type = zap_rd_u16(f + 6) >> 8;
    if (type != (uint16_t)want_type)
        return -1;
    root = zap_rd_u32(f + 8);
    if (root < ZAP_HEADER_SIZE || root >= size)
        return -1;

    *root_out = root;
    *size_out = size;
    return 0;
}

/*
 * Read a text/bytes field at a root object slot, zero-copy. Returns a pointer
 * into f and sets *len_out, or NULL for a null/out-of-range field.
 */
static inline const uint8_t *zap_read_var(const uint8_t *f, uint32_t size,
                                          uint32_t root, int field_off,
                                          uint32_t *len_out) {
    uint32_t slot = root + field_off, rel, len, abs;

    if (slot + 8 > size) { *len_out = 0; return NULL; }
    rel = zap_rd_u32(f + slot);
    if (rel == 0) { *len_out = 0; return NULL; }
    len = zap_rd_u32(f + slot + 4);
    abs = slot + rel;
    if (abs < ZAP_HEADER_SIZE || abs + len > size) { *len_out = 0; return NULL; }

    *len_out = len;
    return f + abs;
}

#endif /* ZAP_PROTOCOL_H */
