/**************************************************************************
** CONSOLE_OUT - control frames to and from the USB host, and where the  **
**               text a command prints ends up                           **
**                                                                       **
** In FORWARDING mode the USB port carries binary frames only. A command **
** that arrives inside a COMMAND control frame (control_frame.h) still   **
** prints with printf like it always did; while it runs, stdio's USB     **
** driver is switched off and a capture driver collects the text, which  **
** leaves as REPLY frames of at most CONSOLE_OUT_CHUNK bytes, the last   **
** one carrying the LAST flag so the host knows the command is done      **
** without waiting for a prompt. Twenty command handlers stay untouched. **
**                                                                       **
** Binary frames to the host (data frames forwarded from the line and    **
** control frames) are written straight to the USB stdio driver with     **
** console_out_usb_write(), never through printf, so they are not        **
** affected by the capture switch and never get CR/LF translation.       **
**                                                                       **
** Nothing here blocks for long: the USB driver drops output when the    **
** host does not read for 500 ms, and that is counted where it can be.   **
***************************************************************************/
#ifndef CONSOLE_OUT_H
#define CONSOLE_OUT_H

#include <stdint.h>
#include <stdbool.h>

#include "control_frame.h"

/** Longest REPLY payload per frame: a whole frame stays within BYTE_FRAME_MAX. */
#define CONSOLE_OUT_CHUNK CONTROL_FRAME_MAX_PAYLOAD

/** Log level the host set with LOG_LEVEL; OFF at boot and after HELLO. There is no
 *  log source in this firmware yet, so the value is stored and reported only. */
extern uint8_t consoleOutLogLevel;

/** STAT push period in ms, 0 = off. Same reset rules as the log level. */
extern uint16_t consoleOutStatPeriodMs;

/** Reset the module. Call once before the tasks start. */
void console_out_init(void);

/** Write raw bytes to the USB host, bypassing printf and CR/LF translation. */
void console_out_usb_write(const uint8_t *buf, int len);

/** Byte-stuff and send one control frame. Returns false (and counts) when it was
 *  too long or could not be built. */
bool console_out_send_frame(uint8_t type, uint8_t id, uint8_t flags, const uint8_t *payload,
                            int payload_len);

/** Handle one parsed control frame from the host (HELLO, COMMAND, LOG_LEVEL,
 *  STAT_PUSH, PING; anything else is answered with a NAK). Runs the command in
 *  the calling task, so call it from the console task only. */
void console_out_handle_control_frame(const control_frame *cf);

/** One event: a text line in DEBUG mode, an EVENT frame otherwise. */
void console_out_event(uint8_t code, const char *text);

/** Send one STAT frame with every counter (field order is part of the protocol,
 *  new fields go at the END only). Does nothing in DEBUG mode. */
void console_out_stat_push(void);

/** Call from the console task loop: sends the periodic STAT when due. */
void console_out_poll(void);

/** Number of control frames that could not be sent. */
uint32_t console_out_dropped(void);

#endif /* CONSOLE_OUT_H */
