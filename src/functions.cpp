#include "functions.h"
#include <stdio.h>

void printFreeRTOSVersion() {
    printf("FreeRTOS version: %d.%d.%d\n", tskKERNEL_VERSION_MAJOR, tskKERNEL_VERSION_MINOR, tskKERNEL_VERSION_BUILD);
}

void printTaskInfo() {

    printf("\r\n");
    
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
