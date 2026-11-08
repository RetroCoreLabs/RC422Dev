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
#include "tusb.h" // tud_cdc_write_available, CFG_TUD_CDC_TX_BUFSIZE (TinyUSB, linked by pico_stdio_usb)

#include "FreeRTOS.h"
#include "task.h"

#include "commands.h"
#include "shared.h"
#include "usb_trace.h"

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

// Timing of USB frame writes. The SDK's USB driver drops output SILENTLY in two cases:
// it could not take the stdio mutex within 1000 ms, or the CDC buffer had no space for
// 500 ms. Both show up as an unusually long write, so the duration is the only evidence
// available from this side. Counted here, shown by STAT and sent in the STAT frame.
// (2026-11-08: 7 of 451 forwarded frames vanished between the decoder and the PC.)
static uint32_t usbWrites = 0;
static uint32_t usbWritesOver450ms = 0; // CDC buffer full for the 500 ms timeout: bytes lost
static uint32_t usbWritesOver900ms = 0; // stdio mutex not obtained in 1000 ms: whole frame lost
static uint32_t usbWriteMaxUs = 0;
// Third silent drop in the SDK driver: stdio_usb_out_chars() returns at once, writing
// nothing, when stdio_usb_connected() (the CDC DTR line state) is false at that moment.
// Found as the only fast whole-frame drop after a 60 s run showed every write under 303 us
// yet 8 of 884 forwarded frames missing at the PC (2026-11-08).
static uint32_t usbWritesNotConnected = 0;
static uint32_t usbWritesNotConnectedAfter = 0; // same check right AFTER the write returned
static uint32_t usbFifoNotEmptyAfterWrite = 0;  // CDC TX FIFO still held bytes after the write returned

// Lag detector state (see console_out_usb_write). The stuffed frame head is the test tool's
// A1 03 + 32-bit sequence number, so a logged entry names the lost frame directly.
#define USB_LAG_LOG_SIZE 32
struct UsbLagEntry
{
    uint32_t ms;
    uint32_t lag;      // bytes handed while connected minus endpoint bytes started, after this write
    uint16_t len;      // stuffed length of this write
    uint16_t fifoFree; // CDC TX FIFO room right after this write
    uint8_t head[8];
};
UsbLagEntry usbLagLog[USB_LAG_LOG_SIZE];
volatile uint32_t usbLagEvents = 0;
static uint32_t usbBytesHandedConnected = 0;
static uint32_t usbLagLast = 0;
static uint32_t usbLagMax = 0;

// USB line events seen by TinyUSB, to tell WHY the Pico ever saw "not connected" during a
// run: the host's DTR going low (SET_CONTROL_LINE_STATE), a bus suspend, or an unmount
// (bus reset). TinyUSB declares these callbacks weak and the SDK's USB stdio does not
// define them, so they are free for this firmware. Called from TinyUSB's device task.
static volatile uint32_t usbDtrLowEvents = 0;   // DTR went 1 -> 0
static volatile uint32_t usbDtrHighEvents = 0;  // DTR went 0 -> 1
static volatile uint32_t usbLastDtrLowMs = 0;   // time of the last DTR drop, ms since boot
static volatile uint32_t usbSuspends = 0;
static volatile uint32_t usbUnmounts = 0;
static volatile bool usbDtrState = false;
// Byte accounting across the USB boundary: bytes handed to the SDK driver (which loops until
// TinyUSB accepted them all) and the number of completed IN transfers reported by TinyUSB.
// Compared with the PC's raw byte count to see on which side of the cable bytes go missing.
static uint32_t usbBytesRequested = 0;
static volatile uint32_t usbTxCompletes = 0;

extern "C" void tud_cdc_tx_complete_cb(uint8_t itf)
{
    (void)itf;
    usbTxCompletes++;
}

extern "C" void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)rts;
    if (dtr && !usbDtrState)
    {
        usbDtrHighEvents++;
    }
    else if (!dtr && usbDtrState)
    {
        usbDtrLowEvents++;
        usbLastDtrLowMs = to_ms_since_boot(get_absolute_time());
    }
    usbDtrState = dtr;
}

extern "C" void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    usbSuspends++;
}

extern "C" void tud_umount_cb(void)
{
    usbUnmounts++;
}

void console_out_usb_line_stats(uint32_t *dtrLow, uint32_t *dtrHigh, uint32_t *lastDtrLowMs,
                                uint32_t *suspends, uint32_t *unmounts)
{
    *dtrLow = usbDtrLowEvents;
    *dtrHigh = usbDtrHighEvents;
    *lastDtrLowMs = usbLastDtrLowMs;
    *suspends = usbSuspends;
    *unmounts = usbUnmounts;
}

void console_out_usb_byte_stats(uint32_t *bytesRequested, uint32_t *txCompletes)
{
    *bytesRequested = usbBytesRequested;
    *txCompletes = usbTxCompletes;
}

void console_out_usb_write(const uint8_t *buf, int len)
{
    // Straight to the USB stdio driver: not through the stdio layer, so the capture
    // switch below and CR/LF translation never touch binary frames. The driver takes
    // its own mutex and waits for CDC buffer space itself.
    absolute_time_t t0 = get_absolute_time();

    bool wasConnected = stdio_usb_connected();
    if (!wasConnected)
    {
        usbWritesNotConnected++; // the driver will drop this whole write silently
    }

    if (stdio_usb.out_chars != NULL)
    {
        stdio_usb.out_chars((const char *)buf, len);
    }
    if (stdio_usb.out_flush != NULL)
    {
        stdio_usb.out_flush();
    }

    uint32_t us = (uint32_t)absolute_time_diff_us(t0, get_absolute_time());
    usbWrites++;
    usbBytesRequested += (uint32_t)len;
    if (!stdio_usb_connected())
    {
        usbWritesNotConnectedAfter++; // "connected" was false right after the write returned
    }
    // After out_chars + out_flush the CDC TX FIFO should be empty (everything moved to transfers,
    // or at most one packet waiting for the endpoint). Bytes still waiting here at the end of a
    // write would be flushed by the completion callback; count how often that state is seen.
    if (tud_cdc_write_available() < CFG_TUD_CDC_TX_BUFSIZE)
    {
        usbFifoNotEmptyAfterWrite++;
    }

    // Lag detector: bytes handed to the driver while connected, minus bytes the endpoint has
    // started. Normally at most one FIFO (64 bytes) plus a packet in flight. A jump by a frame's
    // size that does not recover means that frame's bytes never reached the endpoint; record
    // which write it was (its first bytes carry the test tool's sequence number).
    {
        uint32_t starts, startBytes, completes, completeBytes, failed;
        usb_trace_in_endpoint(USB_TRACE_CDC_EP_IN, &starts, &startBytes, &completes, &completeBytes, &failed);
        usbBytesHandedConnected += wasConnected ? (uint32_t)len : 0u;
        uint32_t lag = usbBytesHandedConnected - startBytes;
        if (lag > usbLagMax)
        {
            usbLagMax = lag;
        }
        if (lag >= usbLagLast + 100u && usbLagEvents < USB_LAG_LOG_SIZE)
        {
            // the lag grew by about a frame since the last write: log this write
            usbLagLog[usbLagEvents].lag = lag;
            usbLagLog[usbLagEvents].len = (uint16_t)len;
            usbLagLog[usbLagEvents].fifoFree = (uint16_t)tud_cdc_write_available();
            usbLagLog[usbLagEvents].ms = to_ms_since_boot(get_absolute_time());
            for (int k = 0; k < 8; k++)
            {
                usbLagLog[usbLagEvents].head[k] = (k < len) ? buf[k] : 0;
            }
            usbLagEvents++;
        }
        usbLagLast = lag;
    }
    if (us > usbWriteMaxUs)
    {
        usbWriteMaxUs = us;
    }
    if (us >= 900000u)
    {
        usbWritesOver900ms++;
    }
    else if (us >= 450000u)
    {
        usbWritesOver450ms++;
    }
}

void console_out_usb_lag_stats(uint32_t *lagMax, uint32_t *lagEvents)
{
    *lagMax = usbLagMax;
    *lagEvents = usbLagEvents;
}

void console_out_usb_write_stats(uint32_t *writes, uint32_t *over450ms, uint32_t *over900ms,
                                 uint32_t *maxUs, uint32_t *notConnected)
{
    *writes = usbWrites;
    *over450ms = usbWritesOver450ms;
    *over900ms = usbWritesOver900ms;
    *maxUs = usbWriteMaxUs;
    *notConnected = usbWritesNotConnected;
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
                 "name=RC422Dev\nversion=%s\nbuild=%s\nmaxframe=%d\ncaps=cmd,stat,event\n",
                 RC422DEV_VERSION, RC422DEV_BUILD_STAMP, BYTE_FRAME_MAX);
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
    uint8_t payload[43 * 4];
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
    // USB write timing, appended 2026-11-08 (see console_out_usb_write)
    put_u32(&payload[4 * i++], usbWrites);
    put_u32(&payload[4 * i++], usbWritesOver450ms);
    put_u32(&payload[4 * i++], usbWritesOver900ms);
    put_u32(&payload[4 * i++], usbWriteMaxUs);
    put_u32(&payload[4 * i++], usbWritesNotConnected); // appended 2026-11-08, field 16
    // USB line events, appended 2026-11-08, fields 17..21
    put_u32(&payload[4 * i++], usbDtrLowEvents);
    put_u32(&payload[4 * i++], usbDtrHighEvents);
    put_u32(&payload[4 * i++], usbLastDtrLowMs);
    put_u32(&payload[4 * i++], usbSuspends);
    put_u32(&payload[4 * i++], usbUnmounts);
    // Byte accounting, appended 2026-11-08, fields 22..23
    put_u32(&payload[4 * i++], usbBytesRequested);
    put_u32(&payload[4 * i++], usbTxCompletes);
    // Inside TinyUSB, CDC data IN endpoint, appended 2026-11-08, fields 24..28
    uint32_t starts, startBytes, completes, completeBytes, failed;
    usb_trace_in_endpoint(USB_TRACE_CDC_EP_IN, &starts, &startBytes, &completes, &completeBytes, &failed);
    put_u32(&payload[4 * i++], starts);
    put_u32(&payload[4 * i++], startBytes);
    put_u32(&payload[4 * i++], completes);
    put_u32(&payload[4 * i++], completeBytes);
    put_u32(&payload[4 * i++], failed);
    // Raw bus events and the after-write connected check, appended 2026-11-08, fields 29..34
    uint32_t suspends, resumes, resets, unplugs, lastSuspendMs;
    usb_trace_bus_events(&suspends, &resumes, &resets, &unplugs, &lastSuspendMs);
    put_u32(&payload[4 * i++], suspends);
    put_u32(&payload[4 * i++], resumes);
    put_u32(&payload[4 * i++], resets);
    put_u32(&payload[4 * i++], unplugs);
    put_u32(&payload[4 * i++], lastSuspendMs);
    put_u32(&payload[4 * i++], usbWritesNotConnectedAfter);
    // CDC FIFO stage, appended 2026-11-08, fields 35..40
    uint32_t wc, req, acc, fc, fb;
    usb_trace_cdc_stage(&wc, &req, &acc, &fc, &fb);
    put_u32(&payload[4 * i++], wc);
    put_u32(&payload[4 * i++], req);
    put_u32(&payload[4 * i++], acc);
    put_u32(&payload[4 * i++], fc);
    put_u32(&payload[4 * i++], fb);
    put_u32(&payload[4 * i++], usbFifoNotEmptyAfterWrite);
    // Lag detector, appended 2026-11-08, fields 41..42
    put_u32(&payload[4 * i++], usbLagMax);
    put_u32(&payload[4 * i++], usbLagEvents);
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
