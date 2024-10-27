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


void initialize_console();

void showPrompt();
void handleSerialInput();
void handleReceiveBufffer();

#endif  // RECEICONSOLE_HVERIRQ_H
