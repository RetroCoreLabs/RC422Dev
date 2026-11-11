#include "commands.h"
#include "RingBuffer.h"
// #include "hdlc.h"
#include <cstdlib>
#include <string.h>
#include "hardware/watchdog.h"
#include "sync.h"
#include "console_out.h"
#include "usb_trace.h"
#include "console.h" // consoleInputStats

// Function to set the current clock speed
bool setTXBaudRate(uint32_t selectedBaud)
{
    if (selectedBaud >= 9600 && selectedBaud <= 983000)
    {
        // Reprogram the running PIO transmitter; before the TX task has started, the task
        // itself picks up currentBaudRate when it initialises the PIO.
        if (txPioReady && !applyTxBaud(selectedBaud))
        {
            printf("Baud rate NOT changed: the transmitter did not go idle within 500 ms\r\n");
            return false;
        }
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
    printf("Build: %s\r\n", RC422DEV_BUILD_STAMP);
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

    // USB writes to the host: the SDK driver drops silently after 500 ms without buffer space
    // (write takes 450..899 ms, bytes lost) or 1000 ms without the stdio mutex (900 ms or more,
    // whole frame lost); the durations are the only evidence of a drop
    // A write attempted while the CDC reports "not connected" (DTR low) is dropped whole and silently
    uint32_t writes, over450, over900, maxUs, notConnected;
    console_out_usb_write_stats(&writes, &over450, &over900, &maxUs, &notConnected);
    printf("USB writes %u, over 450 ms %u, over 900 ms %u, longest %u us, not connected %u, control frames not sent %u\r\n",
           (unsigned)writes, (unsigned)over450, (unsigned)over900, (unsigned)maxUs,
           (unsigned)notConnected, (unsigned)console_out_dropped());

    // Why the USB side was ever "not connected": host DTR transitions, bus suspends, unmounts
    uint32_t dtrLow, dtrHigh, lastDtrLowMs, suspends, unmounts;
    console_out_usb_line_stats(&dtrLow, &dtrHigh, &lastDtrLowMs, &suspends, &unmounts);
    printf("USB line: DTR drops %u (last at %u ms), DTR raises %u, suspends %u, unmounts %u\r\n",
           (unsigned)dtrLow, (unsigned)lastDtrLowMs, (unsigned)dtrHigh, (unsigned)suspends, (unsigned)unmounts);

    // Byte accounting for comparison with the host's raw byte count
    uint32_t bytesRequested, txCompletes;
    console_out_usb_byte_stats(&bytesRequested, &txCompletes);
    printf("USB bytes handed to the driver %u, completed IN transfers %u\r\n",
           (unsigned)bytesRequested, (unsigned)txCompletes);

    // Inside TinyUSB (link-time wraps, usb_trace.cpp): what the device stack started and what the
    // RP2040 hardware reported complete on the CDC data IN endpoint. handed == started == completed
    // means every byte left the Pico; started > completed means the device driver discarded data.
    uint32_t starts, startBytes, completes, completeBytes, failed;
    usb_trace_in_endpoint(USB_TRACE_CDC_EP_IN, &starts, &startBytes, &completes, &completeBytes, &failed);
    printf("USB EP 0x82 IN: started %u transfers / %u bytes, completed %u / %u bytes, failed %u\r\n",
           (unsigned)starts, (unsigned)startBytes, (unsigned)completes, (unsigned)completeBytes, (unsigned)failed);

    // Raw bus events from the USB block (before TinyUSB gates its callbacks). A suspend makes the
    // SDK's write drop whole frames silently while it lasts.
    uint32_t busSuspends, busResumes, busResets, busUnplugs, lastSuspendMs;
    usb_trace_bus_events(&busSuspends, &busResumes, &busResets, &busUnplugs, &lastSuspendMs);
    printf("USB bus: suspends %u (last at %u ms), resumes %u, resets %u, unplugs %u\r\n",
           (unsigned)busSuspends, (unsigned)lastSuspendMs, (unsigned)busResumes, (unsigned)busResets, (unsigned)busUnplugs);

    // The CDC FIFO stage between the SDK write and the endpoint: requested == accepted == flushed
    // means the FIFO passed every byte on; a shortfall says where in that stage bytes vanish
    uint32_t wc, req, acc, fc, fb;
    usb_trace_cdc_stage(&wc, &req, &acc, &fc, &fb);
    printf("USB CDC FIFO: write calls %u, requested %u, accepted %u; flush calls %u, flushed %u bytes\r\n",
           (unsigned)wc, (unsigned)req, (unsigned)acc, (unsigned)fc, (unsigned)fb);

    // Lag detector: handed-while-connected minus endpoint-started, per write; a jump by a frame
    // that stays is a frame whose bytes never reached the endpoint (details in usbLagLog in RAM)
    uint32_t lagMax, lagEvents;
    console_out_usb_lag_stats(&lagMax, &lagEvents);
    printf("USB lag: max %u bytes, jump events %u\r\n", (unsigned)lagMax, (unsigned)lagEvents);

    // Receive path, line -> Pico -> USB (measured, see rxPathStats* in sync.cpp)
    RxPathStats rx;
    rxPathStatsGet(&rx);
    uint32_t avgUs = rx.latencyCount ? (uint32_t)(rx.latencyTotalUs / rx.latencyCount) : 0u;
    uint32_t windowMs = (uint32_t)(rx.windowUs / 1000u);
    uint32_t waitMs = (uint32_t)(rx.waitTotalUs / 1000u);
    printf("RX line: bytes %u, idle (0xFF) %u, other (frames, flags, edges) %u\r\n",
           (unsigned)rx.lineBytes, (unsigned)rx.lineIdleBytes, (unsigned)(rx.lineBytes - rx.lineIdleBytes));
    printf("RX queue: max depth %u of %u\r\n", (unsigned)rx.queueDepthMax, (unsigned)rx.queueSize);
    printf("RX frame to USB: frames %u, max %u us, avg %u us, total %u ms\r\n",
           (unsigned)rx.latencyCount, (unsigned)rx.latencyMaxUs, (unsigned)avgUs,
           (unsigned)(rx.latencyTotalUs / 1000u));
    printf("RX task waiting: %u ms of %u ms window\r\n", (unsigned)waitMs, (unsigned)windowMs);
    printf("RX decode task: busy %u ms of %u ms window\r\n",
           (unsigned)(rx.decodeBusyUs / 1000u), (unsigned)windowMs);
    printf("RX line decoder: ok %u, bad CRC %u, odd bits %u, too long %u, aborted %u, short (ignored) %u\r\n",
           (unsigned)rx.decoder.ok, (unsigned)rx.decoder.bad_crc, (unsigned)rx.decoder.odd_bits,
           (unsigned)rx.decoder.too_long, (unsigned)rx.decoder.aborts, (unsigned)rx.decoder.short_frames);
    printf("RX DMA ring: %u bytes, max backlog %u, overruns %u; PIO RX FIFO overflow %s\r\n",
           (unsigned)rx.dmaRingSize, (unsigned)rx.dmaMaxBacklog, (unsigned)rx.dmaOverruns,
           rx.pioRxStall ? "YES" : "no");

    // Transmit side: sleeps while the PIO TX FIFO was full (each one is an interrupt and a task switch)
    uint32_t txWaits;
    uint64_t txWaitUs;
    txPathStatsGet(&txWaits, &txWaitUs);
    printf("TX FIFO full: waits %u, waiting %u ms\r\n", (unsigned)txWaits, (unsigned)(txWaitUs / 1000u));
    printf("USB RX frames over %u bytes (thrown away): %u\r\n", (unsigned)HDLC::HDLC_MAX_FRAME_LENGTH,
           (unsigned)usbRxOversizeFrames);

    // USB input from the host: block reads, and how the console task woke
    uint32_t inReads, inBytes, wakeUsb, wakeTimeout;
    consoleInputStats(&inReads, &inBytes, &wakeUsb, &wakeTimeout);
    printf("USB RX from host: reads %u, bytes %u; console woke by USB %u, by 10 ms timeout %u\r\n",
           (unsigned)inReads, (unsigned)inBytes, (unsigned)wakeUsb, (unsigned)wakeTimeout);

    // "STAT CLEAR" (after printing) zeroes the SENT/RECEIVED frame table, the RX lines and the PIO
    // receive overflow flag. The USB lines above stay counted from boot: they are cross-checked
    // against each other (handed = accepted = started) and clearing them mid-way would break that.
    // Clear between test runs, not during traffic: the tasks may be counting at the same moment.
    if (args != NULL && strncasecmp(args, "CLEAR", 5) == 0)
    {
        memset(&sentStats, 0, sizeof(sentStats));
        memset(&receivedStats, 0, sizeof(receivedStats));
        rxPathStatsClear();
        txPathStatsClear();
        usbRxOversizeFrames = 0;
        consoleInputStatsClear();
        printf("Cleared: frame table, RX lines, PIO RX overflow flag (RX task totals included). USB lines count from boot.\r\n");
    }
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
    console_out_event(CONTROL_FRAME_EVENT_MODE_CHANGE, "FORWARDING");
}

void handlePacketGen(const char *args)
{
    printf("Switching to Packet Generation mode...\r\n");
    printf("Every character you type will generate an HDLC frame.\r\n");
    printf("Press ESC three times to return to FORWARDING mode.\r\n");
    systemState = PACKET_GEN;
    console_out_event(CONTROL_FRAME_EVENT_MODE_CHANGE, "PACKET_GEN");
}