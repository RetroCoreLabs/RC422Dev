#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"


#include "sync.h"


#define LED_PIN 25  // The onboard LED is connected to GPIO 25
#define BLINK_TASK_PRIORITY ( tskIDLE_PRIORITY + 2 )


extern void initialize_sync_rx();
/*-----------------------------------------------------------*/

void printFreeRTOSVersion() {
    printf("FreeRTOS version: %d.%d.%d\n", tskKERNEL_VERSION_MAJOR, tskKERNEL_VERSION_MINOR, tskKERNEL_VERSION_BUILD);
}

void printTaskInfo() {
    printf("Task Name    State    Priority    Stack    Task ID\n");
    printf("----------------------------------------------------\n");
    TaskStatus_t *pxTaskStatusArray;
    volatile UBaseType_t uxArraySize;
    uxArraySize = uxTaskGetNumberOfTasks();
    pxTaskStatusArray = static_cast<TaskStatus_t*>(pvPortMalloc( uxArraySize * sizeof( TaskStatus_t ) ));    
    if( pxTaskStatusArray != NULL ) {
        uxArraySize = uxTaskGetSystemState( pxTaskStatusArray, uxArraySize, NULL );
        for ( UBaseType_t i = 0; i < uxArraySize; i++ ) {
            printf("%-12s %-8d %-10d %-8u %-8u\n", 
                   pxTaskStatusArray[i].pcTaskName, 
                   pxTaskStatusArray[i].eCurrentState, 
                   pxTaskStatusArray[i].uxCurrentPriority, 
                   pxTaskStatusArray[i].usStackHighWaterMark, 
                   pxTaskStatusArray[i].xTaskNumber);
        }
        vPortFree( pxTaskStatusArray );
    }
}

void printHeapInfo() 
{
        size_t freeHeap = xPortGetFreeHeapSize();
        size_t minHeap = xPortGetMinimumEverFreeHeapSize();
        printf("Free Heap: %u bytes\n", freeHeap);
        printf("Minimum Ever Free Heap: %u bytes\n", minHeap);
}

static void tskBlinky( void *pvParameters )
{
	// Remove compiler warning about unused parameter
	( void ) pvParameters;
    
    const TickType_t xDelay = 500 / portTICK_PERIOD_MS;

	for( ;; )
	{
        vTaskDelay( xDelay );
        gpio_put(LED_PIN, 1);    // Turn LED on
        vTaskDelay( xDelay );
        gpio_put(LED_PIN, 0);    // Turn LED off
	}
}


int main( void )
{
    stdio_init_all();    // Initialize UART for stdio output

    printf("Starting main_blinky.\n");

    printFreeRTOSVersion();    
    printHeapInfo();

    initialize_sync_rx();    

    gpio_init(LED_PIN);                 // Initialize LED GPIO
    gpio_set_dir(LED_PIN, GPIO_OUT);    // Set the LED pin to output mode

    
    xTaskCreate( tskBlinky,				        // The function that implements the task.
                "BlinkyTask", 			        // The text name assigned to the task - for debug only as it is not used by the kernel.
                configMINIMAL_STACK_SIZE,   	// The size of the stack to allocate to the task.
                NULL, 						    // The parameter passed to the task - not used in this case. 
                BLINK_TASK_PRIORITY, 	        // The priority assigned to the task. 
                NULL );						    // The task handle is not required, so NULL is passed.


    // Print task info, should show only the BlinkTask
    printTaskInfo();
    
    // Start scheduler
    vTaskStartScheduler();

	for( ;; );

    return 0;
}

