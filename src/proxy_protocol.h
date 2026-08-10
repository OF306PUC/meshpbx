#ifndef PROXY_PROTOCOL_H
#define PROXY_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Proxy application-layer protocol
 *
 * Embedded in Meshtastic Data.payload.bytes with portnum = PROXY_PORTNUM.
 * The nRF52840 reads the DST_ID to route the message to the correct BLE
 * connection.  The Meshtastic mesh is used purely as transport.
 *
 * Wire format:
 *   [VERSION : 1 byte ]
 *   [SRC_ID  : 4 bytes]   sender   identifier (phone number, UUID, etc.)
 *   [DST_ID  : 4 bytes]   receiver identifier
 *   [content : N bytes ]  actual message payload
 *
 * Total header overhead: PROXY_HEADER_SIZE = 9 bytes.
 *
 * Practical Meshtastic payload limit: ~200 bytes (after mesh headers).
 * Available for content: PROXY_CONTENT_MAX = 191 bytes.
 *
 * ID format (4 bytes, null-padded):
 *   - Phone number (e.g. 942756818)
 *   - Any opaque identifier ≤ 4 bytes
 */

/* Meshtastic portnum reserved for this proxy protocol.
 * meshtastic_PortNum_PRIVATE_APP = 256 */
#define PROXY_PORTNUM           256U

/* Practical Meshtastic Data.payload limit (conservative). */
#define PROXY_PRACTICAL_MAX     200U

#define PROXY_VERSION           0x01U
#define PROXY_ID_SIZE           4U
#define PROXY_HEADER_SIZE       (1U + 2U * PROXY_ID_SIZE)                   /* 9 */
#define PROXY_CONTENT_MAX       (PROXY_PRACTICAL_MAX - PROXY_HEADER_SIZE)   /* 191 */

/* Offsets within the raw payload buffer */
#define PROXY_OFF_VERSION       0U
#define PROXY_OFF_SRC           1U
#define PROXY_OFF_DST           (PROXY_OFF_SRC + PROXY_ID_SIZE) /* 5 */
#define PROXY_OFF_CONTENT       PROXY_HEADER_SIZE               /* 9 */

_Static_assert(PROXY_HEADER_SIZE == 1U + 2U * PROXY_ID_SIZE, 
               "PROXY_HEADER_SIZE must follow PROXY_ID_SIZE"); 

/* A 4-byte proxy identifier (phone number, UUID, etc., null-padded). */
typedef struct {
    uint8_t bytes[PROXY_ID_SIZE];
} proxy_id_t;

/* Parsed proxy header. content points into the original buffer. */
struct proxy_header {
    proxy_id_t     src;          /*(sender) message source*/
    proxy_id_t     dst;          /*(receiver) message destination*/
    const uint8_t *content;      /* pointer into original payload buffer */
    uint16_t       content_len;
};

uint32_t proxy_id_value(const uint8_t *bytes); 

/*
 * Parse a proxy header from raw Data.payload bytes.
 *
 * @param payload  Raw bytes from MeshPacket.decoded.payload.bytes
 * @param len      MeshPacket.decoded.payload.size
 * @param out      Populated on success; content points into payload.
 * @return true on success, false if too short or wrong version.
 */
bool proxy_header_parse(const uint8_t *payload, uint16_t len,
                        struct proxy_header *out);

/*
 * Build a proxy-framed payload into dst_buf.
 * Use this in the phone app (or for loopback tests on the nRF).
 *
 * @return Total bytes written, or 0 if content_len > PROXY_CONTENT_MAX
 *         or dst_buf_size is too small.
 */
uint16_t proxy_header_build(const proxy_id_t *src,
                            const proxy_id_t *dst,
                            const uint8_t    *content,
                            uint16_t          content_len,
                            uint8_t          *dst_buf,
                            uint16_t          dst_buf_size);

/* Returns true if the two IDs are byte-for-byte equal. */
bool proxy_id_equal(const proxy_id_t *a, const proxy_id_t *b);

/* Returns true if the ID is all zeros (unregistered). */
bool proxy_id_is_zero(const proxy_id_t *id);

/* Populate a proxy_id_t from a null-terminated string (phone number, etc.).
 * Strings longer than PROXY_ID_SIZE - 1 are truncated. */
void proxy_id_from_str(proxy_id_t *id, const char *str);

/*
 * Render a 4-byte proxy_id as a canonical string — the phone
 * install-id encoding the Android client registers via NODE_REG. `out` must be
 * at least PROXY_ID_STR_SIZE bytes. Always null-terminates. Returns `out`.
 */
const char *proxy_id_to_str(const proxy_id_t *id, char *out, size_t out_size);

/*
 * Render a parsed proxy header as one human-readable line mirroring the wire
 * layout [version][src_id][dst_id][content] — as text, NOT a hexdump:
 *   "[v01][src=<src_value>][dst=<dst_value>][content=<N> B]"
 * `out` must be at least PROXY_HEADER_STR_SIZE bytes. Always null-terminates.
 * Returns `out`.
 */
const char *proxy_header_to_str(const struct proxy_header *hdr,
                                char *out, size_t out_size);

#endif /* PROXY_PROTOCOL_H */
