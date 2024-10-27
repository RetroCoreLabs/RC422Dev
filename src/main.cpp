//  Shift + Alt + F

#include "FreeRTOS.h"
#include <stdio.h>
#include "pico/stdlib.h"
#include "console.h"
#include "sync.h"
#include "shared.h"
#include "functions.h"

extern void initialize_sync_rx();
/*-----------------------------------------------------------*/

int main(void)
{
    stdio_init_all(); // Initialize UART for stdio output

    printf("\r\n");
    printf("RetroCore RS-422 Device booting..\r\n");

    if (!InitializeSharedObjects())
    {
        panic("Unable to allocate shared objects");
        return -1;
    }

    printf("\r\n");

    printFreeRTOSVersion();
    printHeapInfo();
    
    // Initialise all tasks
    initialize_sync_rx();
    initialize_console();

    // Print task info, should show only the BlinkTask
    printTaskInfo();

    // Start scheduler
    vTaskStartScheduler();

    for (;;)
        ;

    return 0;
}

extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    // Handle stack overflow (e.g., log an error or enter an infinite loop)
    printf("Stack overflow detected in task: %s\n", pcTaskName);

    // Optionally, stop execution here for debugging
    taskDISABLE_INTERRUPTS();
    for (;;) {}  // Halt the program to facilitate debugging
}