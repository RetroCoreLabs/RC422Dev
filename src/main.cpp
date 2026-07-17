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
    
    // Set USB serial to 115200 baud (note: USB CDC typically ignores this, but set it anyway)
    //stdio_usb_init();

    printf("\r\n");
    printf("RetroCore RS-422 Device booting..\r\n");
    printf("Build: %s %s\r\n", __DATE__, __TIME__);
    printf("Using %d core(s)\r\n",configNUMBER_OF_CORES);

    if (!InitializeSharedObjects())
    {
        panic("Unable to allocate shared objects");
        return -1;
    }

    printf("\r\n");

    printFreeRTOSVersion();
    printHeapInfo();
    
    // Initialise all tasks
    initialize_sync_tasks();
    initialize_console_task();

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