/*
 * zap_protocol.h — ZAP binary protocol constants and helpers for hanzo/sql.
 *
 * ZAP (Zero-copy Application Protocol) wire format:
 *   Header (16 bytes):
 *     [0-3]   Magic: "ZAP\0"
 *     [4-5]   Version: uint16 LE (currently 1)
 *     [6-7]   Flags: uint16 LE (message type)
 *     [8-11]  Root offset: uint32 LE
 *     [12-15] Size: uint32 LE
 *   Data segment: objects with typed fields at byte offsets
 */
#ifndef ZAP_PROTOCOL_H
#define ZAP_PROTOCOL_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#define ZAP_MAGIC       "ZAP\0"
#define ZAP_MAGIC_LEN   4
#define ZAP_VERSION     1
#define ZAP_HEADER_SIZE 16

/* Message type IDs (matching hanzo/orm constants) */
#define ZAP_MSG_SQL        300
#define ZAP_MSG_KV         301
#define ZAP_MSG_DATASTORE  302
#define ZAP_MSG_DOCUMENTDB 303

/* Field offsets in ZAP objects */
#define ZAP_FIELD_PATH     4   /* Text: request path */
#define ZAP_FIELD_BODY     12  /* Bytes: JSON body */
#define ZAP_RESP_STATUS    0   /* Uint32: HTTP-style status */
#define ZAP_RESP_BODY      4   /* Bytes: response JSON */

/* Little-endian read helpers */
static inline uint16_t zap_read_u16(const uint8_t *buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static inline uint32_t zap_read_u32(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static inline void zap_write_u16(uint8_t *buf, uint16_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
}

static inline void zap_write_u32(uint8_t *buf, uint32_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
    buf[2] = (val >> 16) & 0xFF;
    buf[3] = (val >> 24) & 0xFF;
}

/* ZAP header structure */
typedef struct {
    uint16_t version;
    uint16_t flags;
    uint32_t root_offset;
    uint32_t size;
    uint16_t msg_type;  /* extracted from flags */
} ZapHeader;

/* Parse a ZAP header. Returns true on success. */
static inline bool zap_parse_header(const uint8_t *data, size_t len, ZapHeader *hdr) {
    if (len < ZAP_HEADER_SIZE)
        return false;
    if (memcmp(data, ZAP_MAGIC, ZAP_MAGIC_LEN) != 0)
        return false;

    hdr->version = zap_read_u16(data + 4);
    if (hdr->version != ZAP_VERSION)
        return false;

    hdr->flags = zap_read_u16(data + 6);
    hdr->root_offset = zap_read_u32(data + 8);
    hdr->size = zap_read_u32(data + 12);
    hdr->msg_type = hdr->flags;

    if (hdr->size > len)
        return false;

    return true;
}

/*
 * Read a Text field from a ZAP object.
 * Text is stored as: relative_offset (i32) + length (u32) at field_offset.
 * Returns pointer into original buffer (zero-copy). Sets *out_len.
 */
static inline const char *zap_read_text(const uint8_t *data, size_t data_len,
                                         uint32_t obj_offset, int field_offset,
                                         uint32_t *out_len) {
    uint32_t pos = obj_offset + field_offset;
    if (pos + 8 > data_len) {
        *out_len = 0;
        return NULL;
    }

    int32_t rel_offset = (int32_t)zap_read_u32(data + pos);
    if (rel_offset == 0) {
        *out_len = 0;
        return NULL;
    }

    uint32_t length = zap_read_u32(data + pos + 4);
    uint32_t abs_pos = pos + rel_offset;

    if (abs_pos + length > data_len) {
        *out_len = 0;
        return NULL;
    }

    *out_len = length;
    return (const char *)(data + abs_pos);
}

/*
 * Read a Bytes field from a ZAP object (same layout as Text).
 */
static inline const uint8_t *zap_read_bytes(const uint8_t *data, size_t data_len,
                                              uint32_t obj_offset, int field_offset,
                                              uint32_t *out_len) {
    return (const uint8_t *)zap_read_text(data, data_len, obj_offset, field_offset, out_len);
}

/*
 * Build a ZAP response message.
 * Caller must free() the returned buffer.
 */
static inline uint8_t *zap_build_response(uint32_t status, const uint8_t *body,
                                            uint32_t body_len, uint32_t *out_size) {
    /* Layout: header(16) + root_object(20) + body_data */
    uint32_t total = ZAP_HEADER_SIZE + 20 + body_len;
    /* Align to 8 bytes */
    total = (total + 7) & ~7;

    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) {
        *out_size = 0;
        return NULL;
    }

    /* Header */
    memcpy(buf, ZAP_MAGIC, ZAP_MAGIC_LEN);
    zap_write_u16(buf + 4, ZAP_VERSION);
    zap_write_u16(buf + 6, 0);  /* flags */
    zap_write_u32(buf + 8, ZAP_HEADER_SIZE);  /* root at offset 16 */
    zap_write_u32(buf + 12, total);

    /* Root object at offset 16 */
    uint32_t root = ZAP_HEADER_SIZE;

    /* Field 0: status (uint32) */
    zap_write_u32(buf + root + ZAP_RESP_STATUS, status);

    /* Field 4: body (bytes) — relative offset + length */
    uint32_t body_offset = 20;  /* relative from field position to body data */
    int32_t rel = (int32_t)(body_offset - ZAP_RESP_BODY);
    zap_write_u32(buf + root + ZAP_RESP_BODY, (uint32_t)rel);
    zap_write_u32(buf + root + ZAP_RESP_BODY + 4, body_len);

    /* Body data at root + 20 */
    if (body && body_len > 0)
        memcpy(buf + root + 20, body, body_len);

    *out_size = total;
    return buf;
}

#endif /* ZAP_PROTOCOL_H */
