#include "settings.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

// ---- Settings in flash ----
// One record, written to the last sector of the flash chip. The program runs from SRAM
// (copy_to_ram), so erasing and programming flash cannot pull the code out from under the CPU; the
// SDK flash routines run from RAM as well. Core 1 is never started, so no lockout is needed: it is
// enough to switch interrupts off for the ~50 ms the erase takes. The receive DMA keeps running
// meanwhile (8 KB ring = 66 ms at 983000 bit/s), the USB driver and the FreeRTOS tick just wait.
//
// A reflash (OpenOCD "program", picotool, UF2 copy) only writes the sectors the new image covers, so
// the saved speed stays across a firmware update.
//
// Not kept on purpose:
//   LOOPBACK, ECHO, HDLC  - bring-up switches for a human at the console; starting every power-up in
//                           the normal state (forwarding, no loopback) is the safe default.
//   LOG_LEVEL, STAT push  - set by the host program for its session; HELLO resets them (see
//                           console_out.h), so a saved value would be thrown away at the first HELLO.
//   HDLC_TX_FILL_BYTES    - a build-time constant, agreed with the ESP32 firmware.
// If a new setting must survive power loss, add it to the record and bump SETTINGS_VERSION.

#define SETTINGS_MAGIC   0x52433432u  // "RC42"
#define SETTINGS_VERSION 1u

struct SettingsRecord
{
    uint32_t magic;
    uint32_t version;
    uint32_t baud;
    uint32_t baudInverted; // ~baud: cheap check that the record was written whole
};

#define SETTINGS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

extern char __flash_binary_end; // end of the program image in flash (linker symbol)

static const SettingsRecord *settingsInFlash()
{
    return (const SettingsRecord *)(XIP_BASE + SETTINGS_FLASH_OFFSET);
}

bool settingsLoadBaud(uint32_t *baud)
{
    const SettingsRecord *r = settingsInFlash();
    if (r->magic != SETTINGS_MAGIC || r->version != SETTINGS_VERSION || r->baudInverted != ~r->baud)
    {
        return false;
    }
    // Same limits as the BAUD command
    if (r->baud < 9600 || r->baud > 983000)
    {
        return false;
    }
    *baud = r->baud;
    return true;
}

bool settingsSaveBaud(uint32_t baud)
{
    uint32_t saved;
    if (settingsLoadBaud(&saved) && saved == baud)
    {
        return true; // already there
    }
    if ((uintptr_t)&__flash_binary_end - XIP_BASE > SETTINGS_FLASH_OFFSET)
    {
        return false; // the program has grown into the settings sector
    }

    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    SettingsRecord rec = {SETTINGS_MAGIC, SETTINGS_VERSION, baud, ~baud};
    memcpy(page, &rec, sizeof(rec));

    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(irq);

    return settingsLoadBaud(&saved) && saved == baud; // read it back
}
