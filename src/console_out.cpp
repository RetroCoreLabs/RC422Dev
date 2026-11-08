/**************************************************************************
** CONSOLE_OUT - control frames to and from the USB host, and where the  **
**               text a command prints ends up. See console_out.h.       **
***************************************************************************/
#include "console_out.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/time.h"

#include "FreeRTOS.h"
#include "task.h"

#include "commands.h"
#include "shared.h"

uint8_t consoleOutLogLevel = CONTROL_FRAME_LOG_OFF;
uint16_t consoleOutStatPeriodMs = 0;

// The reply being collected. Only the console task runs commands, so one set is enough.
static bool inReply = false;
static uint8_t replyId = 0;
static uint8_t chunkCounter = 0;
static uint8_t chunk[CONSOLE_OUT_CHUNK];
static int chunkLen = 0;

static uint32_t droppedFrames = 0;
static TickType_t lastStatTick = 0;

// Scratch for building and stuffing one frame (console task only)
static uint8_t frameBuf[BYTE_FRAME_MAX];
static uint8_t stuffedBuf[2 * BYTE_FRAME_MAX + 2];

/* ------------------------------------------------------------- raw USB output */

void console_out_usb_write(const uint8_t *buf, int len)
{
    // Straight to the USB stdio driver: not through the stdio layer, so the capture
    // switch below and CR/LF translation never touch binary frames. The driver takes
    // its own mutex and waits for CDC buffer space itself.
    if (stdio_usb.out_chars != NULL)
    {
        stdio_usb.out_chars((const char *)buf, len);
    }
    if (stdio_usb.out_flush != NULL)
    {
        stdio_usb.out_flush();
    }
}

bool console_out_send_frame(uint8_t type, uint8_t id, uint8_t flags, const uint8_t *payload,
                            int payload_len)
{
    int n = control_frame_build(type, id, flags, payload, payload_len, frameBuf);
    if (n < 0)
    {
        droppedFrames++;
        return false;
    }
    n = byte_frame_encode(frameBuf, n, stuffedBuf);
    console_out_usb_write(stuffedBuf, n);
    return true;
}

/* ------------------------------------------------- capturing a command's printf */

// Appends to the current reply chunk, sending a full chunk as one REPLY frame.
static void console_out_send_chunk(bool last, bool error)
{
    uint8_t flags = (uint8_t)(chunkCounter << CONTROL_FRAME_CHUNK_SHIFT);

    if (last)
    {
        flags |= CONTROL_FRAME_FLAG_LAST;
    }
    if (error)
    {
        flags |= CONTROL_FRAME_FLAG_ERROR;
    }
    console_out_send_frame(CONTROL_FRAME_REPLY, replyId, flags, chunk, chunkLen);
    chunkCounter = (uint8_t)((chunkCounter + 1u) & 0x0Fu);
    chunkLen = 0;
}

static void console_out_capture_write(const char *s, int n)
{
    while (n > 0)
    {
        int room = CONSOLE_OUT_CHUNK - chunkLen;
        int take = (n < room) ? n : room;

        memcpy(&chunk[chunkLen], s, (size_t)take);
        chunkLen += take;
        s += take;
        n -= take;
        if (chunkLen == CONSOLE_OUT_CHUNK)
        {
            console_out_send_chunk(false, false);
        }
    }
}

// A stdio driver whose only job is to receive what printf produces while a command
// from a control frame runs. No CR/LF translation: commands print "\r\n" themselves.
static void capture_out_chars(const char *buf, int len)
{
    console_out_capture_write(buf, len);
}

static stdio_driver_t captureDriver = {
    capture_out_chars, // out_chars
    NULL,              // out_flush
    NULL,              // in_chars
    NULL,              // set_chars_available_callback
    NULL,              // next
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    false,             // last_ended_with_cr
    false              // crlf_enabled
#endif
};

static void console_out_reply_begin(uint8_t id)
{
    inReply = true;
    replyId = id;
    chunkCounter = 0;
    chunkLen = 0;
    // printf now lands in the chunk buffer instead of the USB port
    stdio_set_driver_enabled(&stdio_usb, false);
    stdio_set_driver_enabled(&captureDriver, true);
}

static void console_out_reply_end(bool error)
{
    if (!inReply)
    {
        return;
    }
    stdio_set_driver_enabled(&captureDriver, false);
    stdio_set_driver_enabled(&stdio_usb, true);
    console_out_send_chunk(true, error); // final chunk, sent even when empty
    inReply = false;
}

/* ---------------------------------------------------------- host messages */

static void console_out_host_reset(void)
{
    consoleOutLogLevel = CONTROL_FRAME_LOG_OFF;
    consoleOutStatPeriodMs = 0;
}

static void console_out_nak(const control_frame *cf, uint8_t reason)
{
    console_out_send_frame(CONTROL_FRAME_NAK, cf->id, CONTROL_FRAME_FLAG_LAST, &reason, 1);
}

static void console_out_handle_hello(const control_frame *cf)
{
    char text[CONSOLE_OUT_CHUNK];
    uint8_t payload[1 + CONSOLE_OUT_CHUNK];
    int n;

    // A fresh host session starts quiet
    console_out_host_reset();

    n = snprintf(text, sizeof(text),
                 "name=RC422Dev\nversion=%s\nbuild=%s %s\nmaxframe=%d\ncaps=cmd,stat,event\n",
                 RC422DEV_VERSION, __DATE__, __TIME__, BYTE_FRAME_MAX);
    if (n < 0)
    {
        n = 0;
    }
    if (n > (int)sizeof(text) - 2) // payload holds the version byte plus the text
    {
        n = (int)sizeof(text) - 2;
    }
    payload[0] = CONTROL_FRAME_VERSION;
    memcpy(&payload[1], text, (size_t)n);
    console_out_send_frame(CONTROL_FRAME_HELLO_REPLY, cf->id, CONTROL_FRAME_FLAG_LAST, payload, 1 + n);
}

static void console_out_handle_command(const control_frame *cf)
{
    static char line[CONTROL_FRAME_MAX_PAYLOAD + 1];

    if (cf->payload_len > CONTROL_FRAME_MAX_PAYLOAD)
    {
        console_out_nak(cf, CONTROL_FRAME_NAK_BAD_LENGTH);
        return;
    }
    memcpy(line, cf->payload, (size_t)cf->payload_len);
    line[cf->payload_len] = '\0';

    console_out_reply_begin(cf->id);
    if (strncasecmp(line, "PACKETGEN", 9) == 0)
    {
        // After PACKETGEN the port stops parsing control frames and ESC ESC ESC is the
        // only way back, so a host may not switch it on through a frame.
        printf("PACKETGEN is not available through a control frame; use the text console.\r\n");
        console_out_reply_end(true);
        return;
    }
    processCommand(line);
    console_out_reply_end(false);
}

void console_out_handle_control_frame(const control_frame *cf)
{
    switch (cf->type)
    {
    case CONTROL_FRAME_HELLO:
        console_out_handle_hello(cf);
        break;

    case CONTROL_FRAME_COMMAND:
        console_out_handle_command(cf);
        break;

    case CONTROL_FRAME_LOG_LEVEL:
        if (cf->payload_len == 1 && cf->payload[0] <= CONTROL_FRAME_LOG_TRACE)
        {
            consoleOutLogLevel = cf->payload[0];
            console_out_send_frame(CONTROL_FRAME_REPLY, cf->id, CONTROL_FRAME_FLAG_LAST, NULL, 0);
        }
        else
        {
            console_out_nak(cf, CONTROL_FRAME_NAK_BAD_LENGTH);
        }
        break;

    case CONTROL_FRAME_STAT_PUSH:
        if (cf->payload_len == 2)
        {
            uint16_t period = (uint16_t)(cf->payload[0] | (cf->payload[1] << 8));
            if (period != 0 && period < 100)
            {
                period = 100;
            }
            consoleOutStatPeriodMs = period;
            lastStatTick = xTaskGetTickCount();
            console_out_send_frame(CONTROL_FRAME_REPLY, cf->id, CONTROL_FRAME_FLAG_LAST, NULL, 0);
        }
        else
        {
            console_out_nak(cf, CONTROL_FRAME_NAK_BAD_LENGTH);
        }
        break;

    case CONTROL_FRAME_PING:
        console_out_send_frame(CONTROL_FRAME_REPLY, cf->id, CONTROL_FRAME_FLAG_LAST, NULL, 0);
        break;

    default:
        console_out_nak(cf, CONTROL_FRAME_NAK_UNKNOWN_TYPE);
        break;
    }
}

/* ------------------------------------------------------- unsolicited frames */

void console_out_event(uint8_t code, const char *text)
{
    if (text == NULL)
    {
        text = "";
    }
    if (systemState == DEBUG)
    {
        printf("[event %u] %s\r\n", (unsigned)code, text);
        return;
    }

    uint8_t payload[CONSOLE_OUT_CHUNK];
    int n = (int)strlen(text);

    if (n > CONSOLE_OUT_CHUNK - 1)
    {
        n = CONSOLE_OUT_CHUNK - 1;
    }
    payload[0] = code;
    memcpy(&payload[1], text, (size_t)n);
    console_out_send_frame(CONTROL_FRAME_EVENT, 0, 0, payload, 1 + n);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)(v >> 24u);
}

void console_out_stat_push(void)
{
    // Field order is part of the protocol (same first eight as HDLCGW); the host
    // accepts any multiple of four bytes, so new fields go at the END only.
    uint8_t payload[12 * 4];
    int i = 0;

    if (systemState == DEBUG)
    {
        return;
    }
    put_u32(&payload[4 * i++], (uint32_t)sentStats.totalFrames);
    put_u32(&payload[4 * i++], (uint32_t)sentStats.okFrames);
    put_u32(&payload[4 * i++], (uint32_t)sentStats.badFrames);
    put_u32(&payload[4 * i++], (uint32_t)sentStats.bytes);
    put_u32(&payload[4 * i++], (uint32_t)receivedStats.totalFrames);
    put_u32(&payload[4 * i++], (uint32_t)receivedStats.okFrames);
    put_u32(&payload[4 * i++], (uint32_t)receivedStats.badFrames);
    put_u32(&payload[4 * i++], (uint32_t)receivedStats.bytes);
    // RC422Dev's own, appended 2026-11-08
    put_u32(&payload[4 * i++], (uint32_t)receivedStats.droppedFrames);  // pool ran dry
    put_u32(&payload[4 * i++], (uint32_t)receivedStats.overflowFrames); // past 128 bytes
    put_u32(&payload[4 * i++], (uint32_t)receivedStats.abortedFrames);  // abort sequence
    put_u32(&payload[4 * i++], droppedFrames);                          // control frames not sent
    console_out_send_frame(CONTROL_FRAME_STAT, 0, 0, payload, 4 * i);
}

void console_out_poll(void)
{
    if (consoleOutStatPeriodMs == 0)
    {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if ((now - lastStatTick) >= pdMS_TO_TICKS(consoleOutStatPeriodMs))
    {
        lastStatTick = now;
        console_out_stat_push();
    }
}

uint32_t console_out_dropped(void)
{
    return droppedFrames;
}

void console_out_init(void)
{
    consoleOutLogLevel = CONTROL_FRAME_LOG_OFF;
    consoleOutStatPeriodMs = 0;
    inReply = false;
    chunkLen = 0;
    droppedFrames = 0;
    lastStatTick = 0;
}
