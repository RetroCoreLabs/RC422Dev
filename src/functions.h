#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include "FreeRTOS.h"
#include "task.h"

void printFreeRTOSVersion();
void printTaskInfo();
void printHeapInfo();

// Hardware facts, the same key names as the HELLO answer of HDLCGW (cores, mhz, flash, ram,
// ramfree, psram) so the PC tool can show one table for every board.
typedef struct
{
    unsigned cores;        // cores on the chip (RP2040: 2; core 1 is not used by this firmware)
    unsigned mhz;          // system clock
    unsigned chipVersion;  // RP2040 silicon version, read from the chip
    unsigned romVersion;   // boot ROM version, read from the chip
    unsigned flashImageBytes; // flash size the image was BUILT for (PICO_FLASH_SIZE_BYTES); the real chip size is not read
    const char *rev;       // "B2" for chip version 2, "B0/B1" for version 1 (the SDK cannot tell those apart)
    char serial[17];       // 64-bit unique ID of the FLASH chip as 16 hex digits (the RP2040 has none); "" if not read
    unsigned ramBytes;     // all SRAM (264 KB)
    unsigned heapSize;     // FreeRTOS heap (ucHeap), part of ramBytes
    unsigned heapFree;     // free now
    unsigned heapLeast;    // least free since boot
    unsigned heapLargest;  // largest free block now
    unsigned psramBytes;   // always 0: the Pico has none
} HardwareFacts;
void initHardwareSerial(); // call once before the scheduler starts: reads the flash chip ID
void getHardwareFacts(HardwareFacts *f);
void printHardwareInfo();

#endif