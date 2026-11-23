#include "functions.h"
#include <stdio.h>
#include <string.h>
#include <pico/time.h>
#include <task.h>
#include <FreeRTOS.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/regs/addressmap.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"
#include "hardware/sync.h"
// #include "hardware/adc.h"

uint64_t FreeRTOSRunTimeTicks;

int64_t alarm_callback(alarm_id_t id, void *user_data)
{

    FreeRTOSRunTimeTicks++;
    return 0;
}

void ConfigureTimerForRunTimeStats(void)
{
    FreeRTOSRunTimeTicks = 0;
    add_alarm_in_us(100, alarm_callback, NULL, false);
}

void printFreeRTOSVersion()
{
    printf("FreeRTOS version: %d.%d.%d\n", tskKERNEL_VERSION_MAJOR, tskKERNEL_VERSION_MINOR, tskKERNEL_VERSION_BUILD);
}

void printTaskInfo()
{
    printf("\r\n");

    printf("Task Name    State    Priority   Stack    Task ID\n");
    printf("---------------------------------------------------\n");
    TaskStatus_t *pxTaskStatusArray;
    volatile UBaseType_t uxArraySize;
    uxArraySize = uxTaskGetNumberOfTasks();
    pxTaskStatusArray = static_cast<TaskStatus_t *>(pvPortMalloc(uxArraySize * sizeof(TaskStatus_t)));
    if (pxTaskStatusArray != NULL)
    {
        uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, NULL);
        for (UBaseType_t i = 0; i < uxArraySize; i++)
        {
            printf("%-12s %-8d %-10d %-8u %-8u\n",
                   pxTaskStatusArray[i].pcTaskName,
                   pxTaskStatusArray[i].eCurrentState,
                   pxTaskStatusArray[i].uxCurrentPriority,
                   pxTaskStatusArray[i].usStackHighWaterMark,
                   pxTaskStatusArray[i].xTaskNumber);
        }
        vPortFree(pxTaskStatusArray);
    }
}

static char hardwareSerial[17] = "";

// Reads the flash chip's unique ID with a flash command; interrupts are off for a moment, so
// this runs once at start-up before the tasks exist (the program runs from RAM, so it is safe).
void initHardwareSerial()
{
    pico_unique_board_id_t id;
    // The flash is out of XIP mode while the command runs: no interrupt may touch it meanwhile
    uint32_t irq = save_and_disable_interrupts();
    pico_get_unique_board_id(&id);
    restore_interrupts(irq);
    for (int i = 0; i < 8; i++)
    {
        snprintf(&hardwareSerial[2 * i], 3, "%02X", id.id[i]);
    }
}

void getHardwareFacts(HardwareFacts *f)
{
    HeapStats_t hs;
    vPortGetHeapStats(&hs);
    f->cores = 2;
    f->mhz = clock_get_hz(clk_sys) / 1000000u;
    f->chipVersion = rp2040_chip_version();
    f->rev = (f->chipVersion == 2) ? "B2" : (f->chipVersion == 1) ? "B0/B1" : "unknown";
    f->romVersion = rp2040_rom_version();
    f->flashImageBytes = PICO_FLASH_SIZE_BYTES;
    memcpy(f->serial, hardwareSerial, sizeof(f->serial));
    f->ramBytes = SRAM_END - SRAM_BASE;
    f->heapSize = configTOTAL_HEAP_SIZE;
    f->heapFree = (unsigned)hs.xAvailableHeapSpaceInBytes;
    f->heapLeast = (unsigned)hs.xMinimumEverFreeBytesRemaining;
    f->heapLargest = (unsigned)hs.xSizeOfLargestFreeBlockInBytes;
    f->psramBytes = 0;
}

void printHardwareInfo()
{
    HardwareFacts f;
    getHardwareFacts(&f);
    printf("CPU: RP2040 rev %s (chip version %u), boot ROM version %u, %u cores (core 1 not used), %u MHz\r\n",
           f.rev, f.chipVersion, f.romVersion, f.cores, f.mhz);
    printf("Serial: %s (unique ID of the flash chip; the RP2040 has none)\r\n", f.serial);
    printf("Flash: image built for %u bytes (the size is set in the build, not read from the chip)\r\n",
           f.flashImageBytes);
    printf("Internal RAM: %u bytes in all; FreeRTOS heap %u bytes, %u free (least %u), largest block %u\r\n",
           f.ramBytes, f.heapSize, f.heapFree, f.heapLeast, f.heapLargest);
    printf("PSRAM: 0 (none on the Pico)\r\n");
}

void printHeapInfo()
{
    HeapStats_t heapStats;

    vPortGetHeapStats(&heapStats);
    printf("HEAP avl: %d, blocks %d, alloc: %d, free: %d\r\n",
        heapStats.xAvailableHeapSpaceInBytes,
        heapStats.xNumberOfFreeBlocks,
        heapStats.xNumberOfSuccessfulAllocations,
        heapStats.xNumberOfSuccessfulFrees
    );

    size_t freeHeap = xPortGetFreeHeapSize();
    size_t minHeap = xPortGetMinimumEverFreeHeapSize();
    printf("Free Heap: %u bytes\n", freeHeap);
    printf("Minimum Ever Free Heap: %u bytes\n", minHeap);
}

void printHeapInfo_min()
{
    size_t freeHeap = xPortGetFreeHeapSize();
    size_t minHeap = xPortGetMinimumEverFreeHeapSize();
    printf("Free Heap: %u bytes\n", freeHeap);
    printf("Minimum Ever Free Heap: %u bytes\n", minHeap);
}

#if _ADC_WORKS_
// https://learnembeddedsystems.co.uk/using-the-rp2040-on-board-temperature-sensor
// https://www.youtube.com/watch?v=dsu-c6kSIe0&ab_channel=LearnEmbeddedSystems

// Cant find the adc.h file when running under FreeRTOS. Fix another day

void enableTemperatureSensor()
{
    // Configure ADC
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);
}

void printTemperature()
{
    uint16_t raw = adc_read();
    const float conversion_factor = 3.3f / (1 << 12);
    float result = raw * conversion_factor;
    float temp = 27 - (result - 0.706) / 0.001721;
    printf("Temperature: %.2f°C\n", temp);
}
#endif