#include "commands.h"
#include "RingBuffer.h"
// #include "hdlc.h"
#include <cstdlib>
#include <string.h>
#include "hardware/watchdog.h"
#include "sync.h"

// Function to set the current clock speed
bool setTXBaudRate(uint32_t selectedBaud)
{
    if (selectedBaud >= 9600 && selectedBaud <= 983000)
    {
        currentBaudRate = selectedBaud;
        return true;
    }
    else
    {
        printf("Invalid baud rate selection!\r\n");
        return false;
    }
}

// Set default send baud rate
void SetDefaultSendBaudRate()
{
    setTXBaudRate(defaultBaudRate);
}

// Define the available commands
Command commands[] = {
    {"HELP", "List all commands and their parameters", handleHelp},
    {"SEND", "SEND <hex bytes> - Send a sequence of hex bytes", handleSend},
    {"BAUD", "BAUD <value> - Set baud rate. BAUD MAX for highest rate.", handleBaud},
    {"LOOPBACK", "LOOPBACK ON|OFF - Enable or disable loopback mode", handleLoopback},
    {"ECHO", "Echo data to serial port", handleEcho},
    {"DUMP", "Dump received buffer", handleDump},
    {"CLEAR", "Clear received buffer", handleClear},
    {".", "Send default debug data", handleSendDebug},
    {"HDLC", "Echo HDLC packets", handleHDLC},
    {"F", "Send HDLC frame", handleSendHDLCFrame},
    {"TASK", "List running tasks", handleTask},
    {"INFO", "Show OS info", handleInfo},
    {"STAT", "Show HDLC stat", handleStat},
    {"RESET", "Reset CPU", handleReset},
    {"INSPECT", "Inspect HDLC frame", handleInspect},
    {"DECODE", "Decode HDLC fram from HEX values", handleDecode}};

// Total number of commands
const int numCommands = sizeof(commands) / sizeof(commands[0]);

// Function to handle the HELP command
void handleHelp(const char *args)
{
    printf("Available commands:\r\n");
    for (int i = 0; i < numCommands; i++)
    {
        printf("  %s: %s\r\n", commands[i].name, commands[i].helpText);
    }
}

// Function to handle the SEND command
void handleSend(const char *args)
{
    if (args == NULL || strlen(args) == 0)
    {
        printf("Usage: SEND <hex bytes>\r\n");
        return;
    }

    // Convert the hex string to bytes and send
    while (*args)
    {
        char byteStr[3] = {args[0], args[1], 0};   // Get two hex chars
        uint8_t data = strtoul(byteStr, NULL, 16); // Convert to a byte

        if (hdlcMode)
        {
            bool isFlag = false;
            if (data == 0x7E) // syn char
            {
                isFlag = true;
            }
            sendHdlcByte(data, isFlag);
        }
        else
        {
            sendBuffer->writeBuffer((unsigned char)data); // Send to buffer
        }
        args += 2; // Move to the next pair of hex digits
    }
    if (hdlcMode)
    {
        printf("Data sent to HDLC output.\r\n");
    }
    else
    {
        printf("Data sent to buffer.\r\n");
    }
}

// Function to handle the BAUD command
void handleBaud(const char *args)
{
    if (args == NULL || strlen(args) == 0)
    {
        printf("Current baud rate: %d\r\n", currentBaudRate);
        return;
    }

    if (strcasecmp(args, "MAX") == 0)
    {
        if (setTXBaudRate(maxBaudRate))
        {
            printf("Baud rate set to MAX (%d)\r\n", currentBaudRate);
        }
        return;
    }

    if (strcasecmp(args, "DEFAULT") == 0)
    {
        if (setTXBaudRate(defaultBaudRate))
        {
            printf("Baud rate set to DEFAULT (%d)\r\n", currentBaudRate);
        }
        return;
    }

    uint32_t baudRate = atoi(args); // Convert argument to integer
    if (setTXBaudRate(baudRate))
    {
        printf("Baud rate set to %d\r\n", currentBaudRate);
    }
}

// Function to handle the LOOPBACK command
void handleLoopback(const char *args)
{
    if (args == NULL || strlen(args) == 0)
    {
        printf("Loopback is currently %s\r\n", loopbackEnabled ? "ON" : "OFF");
        return;
    }

    // No parameter means show current status
    if (args == NULL || strlen(args) == 0)
    {
        printf("Current Loopback mode is: ");
        if (loopbackEnabled)
        {
            printf("ENABLED\r\n");
        }
        else
        {
            printf("DISABLED\r\n");
        }
        return;
    }

    if (strcasecmp(args, "ON") == 0)
    {
        loopbackEnabled = true;
        printf("Loopback mode enabled.\r\n");
    }
    else if (strcasecmp(args, "OFF") == 0)
    {
        loopbackEnabled = false;
        printf("Loopback mode disabled.\r\n");
    }
    else
    {
        printf("Invalid parameter. Use ON or OFF.\r\n");
    }
}

void handleEcho(const char *args)
{
    echoToSerial = !echoToSerial;
}

void handleDump(const char *args)
{
    int len = receiveBuffer->Length();

    if (len == 0)
    {
        printf("Nothing to dump\r\n");
        return;
    }

    printf("Dumping %d bytes\r\n", len);

    for (int i = 0; i < len; i++)
    {
        uint8_t data = receiveBuffer->readBuffer();
        printf("0x%02X ", data);
        if ((i + 1) % 8 == 0)
        {
            printf("\r\n");
        }
    }

    printf("\r\n");
}

void handleClear(const char *args)
{
    printf("Cleared receive buffer\r\n");
    receiveBuffer->Clear();
}

void handleSendDebug(const char *args)
{
    int cnt = 10;

    if (args == NULL || strlen(args) == 0)
    {
        cnt = 10;
    }
    else
    {
        cnt = atoi(args); // Convert argument to integer
    }

    printf("Sending %d bytes\r\n", cnt);

    for (int i = 0; i < cnt; i++)
    {
        while (sendBuffer->isBufferFull())
        {
            vTaskDelay(pdMS_TO_TICKS(10)); // 10ms delay
        }
        sendBuffer->writeBuffer((char)(i & 0xFF)); // Send to buffer
    }
}

void handleHDLC(const char *args)
{
    hdlcMode = !hdlcMode;
    printf("HDLC mode is %s.\r\n", hdlcMode ? "ENABLED" : "DISABLED");
}




// Function to handle the TASK command
void handleTask(const char *args)
{
    printTaskInfo();
}

// Function to handle the TASK command
void handleInfo(const char *args)
{
    printf("Build: %s %s\r\n", __DATE__, __TIME__);
    printFreeRTOSVersion();
    printHeapInfo();
}

// Reset the cpu
void handleReset(const char *args)
{
    watchdog_enable(1, 1); // Start watchdog with a very short timeout
    while (1)
    {
    } // Wait for the watchdog to reset the CPU
}



void handleStat(const char *args)
{    


    // Print the header
    printf("HDLC frames   Total#   OK    BAD     BYTES\r\n");
    printf("------------  ------ ------ ------ ----------  \r\n");

   // Print SENT stats from the struct
    printf("SENT          %6d %6d %6d %10d\r\n",
           sentStats.totalFrames,
           sentStats.okFrames,
           sentStats.badFrames,
           sentStats.bytes);

    // Print RECEIVED stats from the struct
    printf("RECEIVED      %6d %6d %6d %10d\r\n",
           receivedStats.totalFrames,
           receivedStats.okFrames,
           receivedStats.badFrames,
           receivedStats.bytes);
}


void handleInspect(const char *args)
{
    inspectCurrentHdlcFrame();
}

// Function to process incoming commands
void processCommand(char *input)
{
    char *command = strtok(input, " ");
    char *args = strtok(NULL, ""); // The rest is the argument string

    for (int i = 0; i < numCommands; i++)
    {
        // Use strcasecmp() for case-insensitive comparison
        if (strcasecmp(command, commands[i].name) == 0)
        {
            commands[i].handler(args); // Call the command handler
            return;
        }
    }

    printf("Unknown command '%s'. Type HELP for a list of commands.\r\n", command);
}


bool createFrame(HDLC &hdlcFrame, const char *args)
{    
    if (args == NULL || strlen(args) == 0)
    {        
        return false;
    }

     // Calculate the length of the input in bytes
    size_t length = strlen(args) / 2;
    uint8_t *dataArray = (uint8_t*)pvPortMalloc(length);  // Dynamically allocate memory for the byte array

    if (dataArray == NULL)
    {
        printf("Memory allocation failed\r\n");
        return false;
    }

    // Convert the hex string to bytes and add to array
    size_t index = 0;
    while (*args && *(args + 1))  // Check if there are at least two hex digits left
    {
        char byteStr[3] = {args[0], args[1], 0};   // Get two hex chars
        uint8_t data = (uint8_t)strtoul(byteStr, NULL, 16); // Convert to a byte

        dataArray[index++] = data;  // Add data to array
        args += 2; // Move to the next pair of hex digits
    }

    hdlcFrame.CreateFrame(dataArray, length);

    // Free the allocated memory after use
    vPortFree(dataArray); 

    return true;
}


void handleSendHDLCFrame(const char *args)
{


    if (args == NULL || strlen(args) == 0)
    {
        printf("Usage: F <hex bytes>\r\n");
        return;
    }

    HDLC hdlcFrame;
    if (createFrame(hdlcFrame, args))
    {

        hdlc_control_field_t ctrl = hdlcFrame.GetControlField();
        hdlcFrame.DumpControlField(ctrl);
        hdlcFrame.DumpFrame();

        printf("=> Sending frame!\r\n");
        SendHDLCFrame(&hdlcFrame);
    }
    else
    {
        printf("Unable to create frame\r\n");
    }
}

// Function to handle the SEND command
void handleDecode(const char *args)
{
    if (args == NULL || strlen(args) == 0)
    {
        printf("Usage: DECODE <hex bytes>\r\n");
        return;
    }

    HDLC hdlcFrame;
    if (createFrame(hdlcFrame, args))
    {

        hdlc_control_field_t ctrl = hdlcFrame.GetControlField();
        hdlcFrame.DumpControlField(ctrl);
        hdlcFrame.DumpFrame();
    }
    else
    {
        printf("Unable to create frame\r\n");
    }
}