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
    {"DECODE", "Decode HDLC fram from HEX values", handleDecode},
    {"EXIT","Exit debug mode", handleExit},
    {"PACKETGEN", "Switch to packet generation mode", handlePacketGen}
};

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
    printf("HDLC frames   Total#   OK    BAD     BYTES  DROPPED OVERFLOW  ABORT\r\n");
    printf("------------  ------ ------ ------ ---------- ------- -------- ------\r\n");

   // Print SENT stats from the struct
    printf("SENT          %6d %6d %6d %10d %7d %8d %6d\r\n",
           sentStats.totalFrames,
           sentStats.okFrames,
           sentStats.badFrames,
           sentStats.bytes,
           sentStats.droppedFrames,
           sentStats.overflowFrames,
           sentStats.abortedFrames);

    // Print RECEIVED stats from the struct
    // DROPPED  = complete frames lost because no free frame object was left
    // OVERFLOW = of the BAD frames, those that grew past HDLC_MAX_FRAME_LENGTH (a missed flag merges two frames)
    // ABORT    = of the BAD frames, those cut short by seven or more ones inside the frame
    printf("RECEIVED      %6d %6d %6d %10d %7d %8d %6d\r\n",
           receivedStats.totalFrames,
           receivedStats.okFrames,
           receivedStats.badFrames,
           receivedStats.bytes,
           receivedStats.droppedFrames,
           receivedStats.overflowFrames,
           receivedStats.abortedFrames);
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

    // Upper bound on the number of bytes: every byte needs two hex digits.
    // (The old code used strlen/2 as the length AND read the string two characters at a time
    // INCLUDING the spaces, so "01 03 48 49" became 01 00 03 48 04. Found 2026-11-08 in the
    // first Pico-to-ESP32 test.)
    size_t maxBytes = strlen(args) / 2 + 1;
    uint8_t *dataArray = (uint8_t*)pvPortMalloc(maxBytes);  // Dynamically allocate memory for the byte array

    if (dataArray == NULL)
    {
        printf("Memory allocation failed\r\n");
        return false;
    }

    // Convert the hex string to bytes. Spaces, tabs and commas between bytes are skipped,
    // so both "01 03 48 49" and "01034849" work. Anything else is an error.
    size_t index = 0;
    int nibbles = 0;      // hex digits seen in the current byte (0 or 1)
    uint8_t current = 0;
    bool ok = true;
    for (const char *p = args; *p != '\0'; p++)
    {
        char c = *p;
        if (c == ' ' || c == '\t' || c == ',')
        {
            if (nibbles != 0)
            {
                printf("Odd number of hex digits before '%c'\r\n", c);
                ok = false;
                break;
            }
            continue;
        }

        int value;
        if (c >= '0' && c <= '9')       value = c - '0';
        else if (c >= 'a' && c <= 'f')  value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')  value = c - 'A' + 10;
        else
        {
            printf("Not a hex digit: '%c'\r\n", c);
            ok = false;
            break;
        }

        current = (uint8_t)((current << 4) | value);
        nibbles++;
        if (nibbles == 2)
        {
            dataArray[index++] = current;
            current = 0;
            nibbles = 0;
        }
    }

    if (ok && nibbles != 0)
    {
        printf("Odd number of hex digits\r\n");
        ok = false;
    }

    if (ok && index == 0)
    {
        printf("No bytes given\r\n");
        ok = false;
    }

    if (ok)
    {
        hdlcFrame.CreateFrame(dataArray, index);
    }

    // Free the allocated memory after use
    vPortFree(dataArray);

    return ok;
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

void handleExit(const char *args)
{
    printf("Exiting debug mode...\r\n");
    systemState =FORWARDING;
}

void handlePacketGen(const char *args)
{
    printf("Switching to Packet Generation mode...\r\n");
    printf("Every character you type will generate an HDLC frame.\r\n");
    printf("Press ESC three times to return to FORWARDING mode.\r\n");
    systemState = PACKET_GEN;
}