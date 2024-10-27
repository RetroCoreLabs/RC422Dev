#include "functions.h"
#include <stdio.h>
#include <pico/time.h>
#include <task.h>
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

void printHeapInfo()
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