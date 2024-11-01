#ifndef COMMANDS_H
#define COMMANDS_H


#include "FreeRTOS.h"
#include "shared.h"
#include "functions.h"

// Typedef for the command handler function pointer
typedef void (*CommandHandler)(const char* args);

// Struct to represent a command
struct Command {
  const char* name;
  const char* helpText;
  CommandHandler handler;
};


// Function prototypes for command handlers
void handleHelp(const char* args);
void handleSend(const char* args);
void handleBaud(const char* args);
void handleLoopback(const char* args);
void handleEcho(const char* args);
void handleDump(const char* args);
void handleClear(const char* args);
void handleSendDebug(const char* args);
void handleHDLC(const char* args);
void handleSendHDLCFrame(const char* args);
void handleTask(const char* args);
void handleStat(const char *args);
void handleInfo(const char* args);
void handleReset(const char* args);
void handleInspect(const char* args);
void handleDecode(const char* args);

// Declare functions for baud rate handling
void SetDefaultSendBaudRate();

// Function to process incoming commands
void processCommand(char* input);


#endif  // COMMANDS_H
