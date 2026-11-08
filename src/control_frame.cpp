/**************************************************************************
** CONTROL_FRAME - messages between the host PC and this firmware        **
** See control_frame.h. Must agree byte for byte with the HDLCGW (ESP32) **
** firmware and with ControlFrame.cs in HdlcGatewayTest.                 **
***************************************************************************/
#include "control_frame.h"

uint16_t control_frame_crc(const uint8_t *frame, int len)
{
    // CRC-16/X.25 bit by bit: same result as the table in hdlc.cpp (RFC 1662 fcstab)
    uint16_t crc = 0xFFFF;

    for (int i = 0; i < len; i++)
    {
        crc ^= frame[i];
        for (int b = 0; b < 8; b++)
        {
            if (crc & 1)
            {
                crc = (uint16_t)((crc >> 1) ^ 0x8408);
            }
            else
            {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}

bool control_frame_parse(const uint8_t *frame, int len, control_frame *out)
{
    if (len < CONTROL_FRAME_OVERHEAD || frame[0] != CONTROL_FRAME_ADDRESS)
    {
        return false;
    }
    if (control_frame_crc(frame, len) != CONTROL_FRAME_RESIDUE)
    {
        return false;
    }

    out->type = frame[1];
    out->id = frame[2];
    out->flags = frame[3];
    out->payload = &frame[CONTROL_FRAME_HEADER];
    out->payload_len = len - CONTROL_FRAME_OVERHEAD;
    return true;
}

int control_frame_build(uint8_t type, uint8_t id, uint8_t flags, const uint8_t *payload,
                        int payload_len, uint8_t *out)
{
    int n = 0;
    uint16_t crc;

    if (payload_len < 0 || payload_len > CONTROL_FRAME_MAX_PAYLOAD)
    {
        return -1;
    }

    out[n++] = CONTROL_FRAME_ADDRESS;
    out[n++] = type;
    out[n++] = id;
    out[n++] = flags;
    for (int i = 0; i < payload_len; i++)
    {
        out[n++] = payload[i];
    }
    // Stored WITHOUT inversion on purpose: that is the second lock, see the header.
    crc = control_frame_crc(out, n);
    out[n++] = (uint8_t)(crc & 0xFFu);
    out[n++] = (uint8_t)(crc >> 8u);
    return n;
}

int byte_frame_encode(const uint8_t *frame, int len, uint8_t *out)
{
    int n = 0;

    out[n++] = 0x7E; // opening flag
    for (int i = 0; i < len; i++)
    {
        uint8_t b = frame[i];
        if (b == 0x7E || b == 0x7D || b < 0x20)
        {
            out[n++] = 0x7D;
            out[n++] = (uint8_t)(b ^ 0x20);
        }
        else
        {
            out[n++] = b;
        }
    }
    out[n++] = 0x7E; // closing flag
    return n;
}
