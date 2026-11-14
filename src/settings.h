#ifndef RC422DEV_SETTINGS_H
#define RC422DEV_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

// Settings that survive a power loss, kept in the last 4 KB sector of the Pico's flash (far from
// the program, which sits at the start). Today only the line speed is kept; see settings.cpp for
// why the other runtime switches (LOOPBACK, ECHO, HDLC, the host's log level and STAT push) are not.

/** Read the saved line speed. Returns false when nothing valid is saved (blank flash, wrong magic
 *  or version, failed check): the caller keeps its built-in default. Safe before the scheduler runs. */
bool settingsLoadBaud(uint32_t *baud);

/** Save the line speed. Does nothing (and returns true) when the same value is already saved, so a
 *  repeated BAUD command costs no flash wear. Erases and writes one sector with interrupts off, which
 *  takes about 50 ms: call it from a command, never from a hot path. Returns false if the write
 *  failed or the sector would overlap the program. */
bool settingsSaveBaud(uint32_t baud);

#endif
