/*
 * proto_handler.c — nanopb decode of Meshtastic FromRadio messages
 *
 * Extracts the fields needed for routing:
 *   - which_payload_variant
 *   - packet.from / packet.to
 *   - packet.decoded.portnum
 *   - packet.decoded.payload.bytes / .size
 *
 * The node decrypts packets before sending over serial, so
 * MeshPacket.which_payload_variant == decoded_tag for all
 * application-layer traffic (see Meshtastic StreamAPI docs).
 *
 * FromRadio raw bytes are NOT re-encoded — forwarded as-is to BLE by the
 * router. The only message the proxy synthesizes (encodes) is the closing
 * FromRadio{config_complete_id} of the per-phone replay; that is built with
 * nanopb pb_encode (never hand-written wire bytes).
 *
 * Threading: every decode/encode entry point uses a module-level static
 * scratch struct (too large for any stack). This is safe ONLY because all
 * decode/cache processing runs on the single Zephyr system work queue —
 * there is no concurrent caller. Do NOT call these from an ISR or a second
 * thread.
 */

#include "proto_handler.h"

#include <string.h>
#include <errno.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include "meshtastic/mesh.pb.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(proto_handler, LOG_LEVEL_DBG);

/*
 * Module-level statics: each too large (~2 KB) for any stack. Kept separate
 * (FromRadio vs ToRadio) so a ToRadio decode never clobbers a FromRadio
 * decode in flight. Safe because the single system work queue serializes all
 * access — see the threading note in the file header.
 */
static meshtastic_FromRadio s_fromradio;
static meshtastic_ToRadio   s_toradio;

int proto_decode_fromradio(const uint8_t *buf, uint16_t len,
                           struct fromradio_info *out)
{
    memset(&s_fromradio, 0, sizeof(s_fromradio));
    memset(out, 0, sizeof(*out));

    pb_istream_t stream = pb_istream_from_buffer(buf, len);
    if (!pb_decode(&stream, meshtastic_FromRadio_fields, &s_fromradio)) {
        LOG_WRN("pb_decode: %s", PB_GET_ERROR(&stream));
        return -EINVAL;
    }

    out->which_variant = s_fromradio.which_payload_variant;

    if (s_fromradio.which_payload_variant != meshtastic_FromRadio_packet_tag) {
        /* Config, NodeInfo, MyInfo, Channel, ConfigComplete, etc. */
        LOG_DBG("variant=%d (config/meta)", out->which_variant);
        return 0;
    }

    /* It's a MeshPacket — extract routing fields. */
    out->packet_from = s_fromradio.packet.from;
    out->packet_to   = s_fromradio.packet.to;

    if (s_fromradio.packet.which_payload_variant !=
        meshtastic_MeshPacket_decoded_tag) {
        /* Encrypted packet — cannot inspect payload. */
        out->is_decoded = false;
        LOG_DBG("packet from=0x%08x to=0x%08x (encrypted)",
                out->packet_from, out->packet_to);
        return 0;
    }

    /* Decoded (plaintext) packet — expose portnum + payload. */
    out->is_decoded    = true;
    out->portnum       = s_fromradio.packet.decoded.portnum;
    out->payload_bytes = s_fromradio.packet.decoded.payload.bytes;
    out->payload_len   = (uint16_t)s_fromradio.packet.decoded.payload.size;

    LOG_DBG("packet from=0x%08x to=0x%08x portnum=%u payload=%u B",
            out->packet_from, out->packet_to,
            out->portnum, out->payload_len);

    return 0;
}

int proto_decode_toradio(const uint8_t *buf, uint16_t len,
                         struct toradio_info *out)
{
    /* See file header: s_toradio is reused per call — single-threaded only. */
    memset(&s_toradio, 0, sizeof(s_toradio));
    memset(out, 0, sizeof(*out));

    pb_istream_t stream = pb_istream_from_buffer(buf, len);
    if (!pb_decode(&stream, meshtastic_ToRadio_fields, &s_toradio)) {
        LOG_WRN("ToRadio pb_decode: %s", PB_GET_ERROR(&stream));
        return -EINVAL;
    }

    switch (s_toradio.which_payload_variant) {
    case meshtastic_ToRadio_want_config_id_tag:
        out->has_want_config = true;
        out->want_config_id  = s_toradio.want_config_id;
        LOG_DBG("ToRadio want_config_id=%u", out->want_config_id);
        break;
    case meshtastic_ToRadio_disconnect_tag:
        out->has_disconnect = true;
        LOG_DBG("ToRadio disconnect");
        break;
    case meshtastic_ToRadio_heartbeat_tag:
        out->has_heartbeat = true;
        LOG_DBG("ToRadio heartbeat");
        break;
    default:
        /* packet / xmodem / mqtt / empty — caller forwards. */
        if (s_toradio.which_payload_variant == meshtastic_ToRadio_packet_tag &&
            s_toradio.packet.which_payload_variant ==
                meshtastic_MeshPacket_decoded_tag) {
            /* Decoded phone->radio packet: expose portnum + payload so the
             * forward path can trace the proxy wire header. Bytes are still
             * forwarded verbatim by the caller. */
            out->is_packet     = true;
            out->portnum       = s_toradio.packet.decoded.portnum;
            out->payload_bytes = s_toradio.packet.decoded.payload.bytes;
            out->payload_len   = (uint16_t)s_toradio.packet.decoded.payload.size;
        }

        if (out->is_packet) {
            LOG_DBG("ToRadio variant=%d, portnum=%d (passthrough)",
                s_toradio.which_payload_variant,
                s_toradio.packet.decoded.portnum);
        } else {
            LOG_DBG("ToRadio variant=%d (passthrough)",
                s_toradio.which_payload_variant);
        }
        break;
    }

    return 0;
}

int proto_encode_config_complete(uint32_t nonce, uint8_t *out_buf,
                                 uint16_t buf_size, uint16_t *out_len)
{
    /* Build the FromRadio entirely via nanopb — no hand-written wire bytes. */
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    fr.config_complete_id    = nonce;

    pb_ostream_t stream = pb_ostream_from_buffer(out_buf, buf_size);
    if (!pb_encode(&stream, meshtastic_FromRadio_fields, &fr)) {
        /* nanopb reports a buffer-too-small condition here as well; map it to
         * -ENOMEM so callers can distinguish it from a structural failure. */
        if (stream.bytes_written >= buf_size) {
            LOG_WRN("config_complete encode: buffer too small (%u B)",
                    buf_size);
            return -ENOMEM;
        }
        LOG_WRN("config_complete pb_encode: %s", PB_GET_ERROR(&stream));
        return -EINVAL;
    }

    *out_len = (uint16_t)stream.bytes_written;
    LOG_DBG("config_complete encoded nonce=%u -> %u B", nonce, *out_len);

    return 0;
}

int proto_encode_heartbeat(uint32_t nonce, uint8_t *out_buf,
                           uint16_t buf_size, uint16_t *out_len)
{
    /* ToRadio{ heartbeat = { nonce } } via nanopb — no hand-written bytes. */
    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    tr.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
    tr.heartbeat.nonce       = nonce;

    pb_ostream_t stream = pb_ostream_from_buffer(out_buf, buf_size);
    if (!pb_encode(&stream, meshtastic_ToRadio_fields, &tr)) {
        if (stream.bytes_written >= buf_size) {
            LOG_WRN("heartbeat encode: buffer too small (%u B)", buf_size);
            return -ENOMEM;
        }
        LOG_WRN("heartbeat pb_encode: %s", PB_GET_ERROR(&stream));
        return -EINVAL;
    }

    *out_len = (uint16_t)stream.bytes_written;
    LOG_DBG("heartbeat encoded nonce=%u -> %u B", nonce, *out_len);

    return 0;
}

int proto_encode_queue_status(uint8_t *out_buf, uint16_t buf_size,
                              uint16_t *out_len)
{
    /* FromRadio{ queueStatus } via nanopb. Benign, well-formed values: success
     * (res=0), some free/maxlen capacity, no specific mesh_packet_id. Only used
     * for BLE heartbeat liveness — the phone just needs valid radio data. */
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant      = meshtastic_FromRadio_queueStatus_tag;
    fr.queueStatus.res            = 0;
    fr.queueStatus.free           = 16;
    fr.queueStatus.maxlen         = 16;
    fr.queueStatus.mesh_packet_id = 0;

    pb_ostream_t stream = pb_ostream_from_buffer(out_buf, buf_size);
    if (!pb_encode(&stream, meshtastic_FromRadio_fields, &fr)) {
        if (stream.bytes_written >= buf_size) {
            LOG_WRN("queueStatus encode: buffer too small (%u B)", buf_size);
            return -ENOMEM;
        }
        LOG_WRN("queueStatus pb_encode: %s", PB_GET_ERROR(&stream));
        return -EINVAL;
    }

    *out_len = (uint16_t)stream.bytes_written;
    LOG_DBG("queueStatus encoded -> %u B", *out_len);

    return 0;
}
