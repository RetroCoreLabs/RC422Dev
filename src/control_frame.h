/**************************************************************************
** CONTROL_FRAME - messages between the host PC and this firmware,       **
**                 carried on the same byte-stuffed USB channel as the   **
**                 data frames                                           **
**                                                                       **
** Same wire format as the HDLCGW (ESP32) gateway, byte for byte; the    **
** PC tool (HdlcGatewayTest, ControlFrame.cs) speaks it to both boards.  **
** Decided by Ronny 2026-11-08: a host never needs the ESC ESC ESC text  **
** console; it sends control frames instead, and they never reach the   **
** line.                                                                 **
**                                                                       **
** HOW A CONTROL FRAME IS TOLD FROM A DATA FRAME: two locks.             **
**  1. The HDLC address byte is CONTROL_FRAME_ADDRESS (0xFE). ISO 13239  **
**     defines 0x00 (no station) and 0xFF (all stations); X.25 / ND LAPB **
**     links use 0x01 and 0x03. 0xFE is none of those.                   **
**  2. The two check bytes at the end are the CRC-16/X.25 register       **
**     stored WITHOUT the usual inversion. Running the CRC over a good   **
**     HDLC frame (FCS included) always leaves 0xF0B8 in the register;   **
**     running it over a control frame always leaves 0x0000. No byte     **
**     string satisfies both, so a data frame with a good FCS can never  **
**     be taken for a control frame even if it is addressed to 0xFE.     **
**                                                                       **
** Layout inside the byte stuffing (little-endian where it matters):     **
**   0      address      0xFE                                            **
**   1      type         bit 7 clear = host -> firmware, set = reply     **
**   2      id           request id 1..255 chosen by the host and copied **
**                       into every reply chunk; 0 = unsolicited          **
**   3      flags        bit 0 LAST, bit 1 ERROR, bits 4..7 chunk count   **
**   4..    payload      up to CONTROL_FRAME_MAX_PAYLOAD bytes            **
**   last 2 check        un-inverted CRC, low byte first                  **
**                                                                       **
** Also here: the RFC 1662 byte stuffing used on the USB port for every  **
** frame (flag 0x7E, escape 0x7D, XOR 0x20; control characters below    **
** 0x20 are escaped too, as the Pico always did).                        **
***************************************************************************/
#ifndef CONTROL_FRAME_H
#define CONTROL_FRAME_H

#include <stdint.h>
#include <stdbool.h>

/** Largest un-stuffed CONTROL frame (protocol limit, shared with the HDLCGW
 *  firmware and ControlFrame.cs). Data frames to and from the line may be longer:
 *  up to HDLC::HDLC_MAX_FRAME_LENGTH (1024 since 2026-11-09), which HELLO reports
 *  as maxframe. */
#define BYTE_FRAME_MAX 128

#define CONTROL_FRAME_ADDRESS   0xFEu   /**< HDLC address that marks a control frame */
#define CONTROL_FRAME_VERSION   1u      /**< protocol version reported in HELLO_REPLY */
#define CONTROL_FRAME_HEADER    4       /**< address, type, id, flags */
#define CONTROL_FRAME_CHECK     2       /**< the un-inverted CRC */
#define CONTROL_FRAME_OVERHEAD  (CONTROL_FRAME_HEADER + CONTROL_FRAME_CHECK)
/** Longest payload that still fits in BYTE_FRAME_MAX. */
#define CONTROL_FRAME_MAX_PAYLOAD (BYTE_FRAME_MAX - CONTROL_FRAME_OVERHEAD)
/** CRC register after a control frame including its check bytes. */
#define CONTROL_FRAME_RESIDUE   0x0000u
/** CRC register after a data frame with a good FCS. */
#define CONTROL_FRAME_HDLC_GOOD 0xF0B8u

/* Message types, host to firmware (bit 7 clear) */
#define CONTROL_FRAME_HELLO      0x01u  /**< identify; resets log level and stat push to off */
#define CONTROL_FRAME_COMMAND    0x02u  /**< payload: a console command line, no CR/LF */
#define CONTROL_FRAME_LOG_LEVEL  0x03u  /**< payload: one byte, 0 off .. 5 trace */
#define CONTROL_FRAME_STAT_PUSH  0x04u  /**< payload: u16 period in ms, 0 = off, min 100 */
#define CONTROL_FRAME_PING       0x05u  /**< empty; answered with an empty REPLY */

/* Message types, firmware to host (bit 7 set) */
#define CONTROL_FRAME_HELLO_REPLY 0x81u /**< version byte, then key=value lines */
#define CONTROL_FRAME_REPLY       0x82u /**< text produced by a command, chunked */
#define CONTROL_FRAME_LOG         0x83u /**< id 0: level, u32 millis, text */
#define CONTROL_FRAME_STAT        0x84u /**< id 0: list of u32 counters */
#define CONTROL_FRAME_EVENT       0x85u /**< id 0: event code, then text */
#define CONTROL_FRAME_NAK         0x86u /**< reason byte, then text */

/* Flags */
#define CONTROL_FRAME_FLAG_LAST   0x01u /**< final chunk of a reply */
#define CONTROL_FRAME_FLAG_ERROR  0x02u /**< the command failed or was refused */
#define CONTROL_FRAME_CHUNK_SHIFT 4u    /**< chunk counter lives in bits 4..7 */

/* Log levels (LOG_LEVEL payload and LOG first byte) */
#define CONTROL_FRAME_LOG_OFF   0u
#define CONTROL_FRAME_LOG_ERROR 1u
#define CONTROL_FRAME_LOG_WARN  2u
#define CONTROL_FRAME_LOG_INFO  3u
#define CONTROL_FRAME_LOG_DEBUG 4u
#define CONTROL_FRAME_LOG_TRACE 5u

/* Event codes (EVENT first byte) */
#define CONTROL_FRAME_EVENT_BOOT           1u  /**< firmware started; text = banner */
#define CONTROL_FRAME_EVENT_MODE_CHANGE    2u  /**< text = FORWARDING / DEBUG / PACKET_GEN */
#define CONTROL_FRAME_EVENT_LINE_RX_ACTIVE 3u  /**< first byte from the line after idle */
#define CONTROL_FRAME_EVENT_LINE_RX_IDLE   4u  /**< no byte from the line for 2 s */
#define CONTROL_FRAME_EVENT_CLOCK_ON       5u
#define CONTROL_FRAME_EVENT_CLOCK_OFF      6u
#define CONTROL_FRAME_EVENT_USB_HOST_LOST  7u

/* NAK reasons */
#define CONTROL_FRAME_NAK_UNKNOWN_TYPE 1u
#define CONTROL_FRAME_NAK_BAD_LENGTH   2u
#define CONTROL_FRAME_NAK_REFUSED      3u

/** A parsed control frame. payload points into the caller's buffer. */
typedef struct
{
    uint8_t type;           /**< CONTROL_FRAME_* message type */
    uint8_t id;             /**< request id, 0 = unsolicited */
    uint8_t flags;          /**< CONTROL_FRAME_FLAG_* and the chunk counter */
    const uint8_t *payload; /**< first payload byte, valid while the frame buffer is */
    int payload_len;        /**< 0..CONTROL_FRAME_MAX_PAYLOAD */
} control_frame;

/** CRC-16/X.25 register after running over every byte (init 0xFFFF, reflected 0x8408,
 *  NOT inverted). The same CRC the HDLC codec uses for the FCS. */
uint16_t control_frame_crc(const uint8_t *frame, int len);

/** Whether a frame is a control frame (both locks hold); fills out when it is.
 *  The type is not checked: an unknown type is still a control frame and is
 *  answered with a NAK rather than clocked onto the line. */
bool control_frame_parse(const uint8_t *frame, int len, control_frame *out);

/** Build a control frame (header, payload, un-inverted CRC) into out, which must
 *  hold payload_len + CONTROL_FRAME_OVERHEAD bytes. Returns the length, or -1. */
int control_frame_build(uint8_t type, uint8_t id, uint8_t flags, const uint8_t *payload,
                        int payload_len, uint8_t *out);

/** RFC 1662 byte stuffing for the USB port: opening flag, escaped bytes, closing
 *  flag. out must hold 2 * len + 2 bytes. Returns the number of bytes written. */
int byte_frame_encode(const uint8_t *frame, int len, uint8_t *out);

#endif /* CONTROL_FRAME_H */
