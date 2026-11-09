#ifndef CONSOLE_H
#define CONSOLE_H

#include "FreeRTOS.h"
#include <semphr.h>
#include <stdio.h>
#include <string.h>  // For C

#include "RingBuffer.h"
#include "commands.h"

#define CONSOLE_TASK_PRIORITY ( tskIDLE_PRIORITY + 2 )


void consoleTask(void *pvParameters);  // Task for receiving SPI data


void initialize_console_task();

void showPrompt();
void handleSerialInput();
void handleReceiveBufffer();

// USB input measurements for STAT: block reads and bytes from the host, and how the console task
// woke (USB "bytes arrived" callback, or its 10 ms ceiling)
void consoleInputStats(uint32_t *reads, uint32_t *bytes, uint32_t *wakeUsb, uint32_t *wakeTimeout);
void consoleInputStatsClear();

#endif  // RECEICONSOLE_HVERIRQ_H
