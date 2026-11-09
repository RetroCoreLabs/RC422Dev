#include "console.h"
#include "shared.h"
#include "hardware/gpio.h"
#include <pico/stdio.h>
#include "pico/stdlib.h"
#include "sync.h"
#include "hdlc.h"
#include "console_out.h"

#if false
#include "commands.h"
#include "sender.h"
#include "receiver.h"
#include "receiverI2S.h"
#include "receiverIRQ.h"
#include "hdlc.h"

#endif

#define LED_PIN 25 // Pin 25 on the pi pico rp2040
// Variable to track the LED state
bool ledState = true;

// Function to toggle the LED
void toggleLED()
{
    ledState = !ledState;        // Toggle the state
    gpio_put(LED_PIN, ledState); // Set the new state to the LED pin
}

void handleReceiveBufffer()
{
    int cnt = 0;

    if (echoToSerial || loopbackEnabled || hdlcMode)
    {
        while (!receiveBuffer->isBufferEmpty())
        {
            cnt++;
            uint8_t data = receiveBuffer->readBuffer();
            if (loopbackEnabled)
            {
                sendBuffer->writeBuffer(data); // Send received data to sender
            }

            if (hdlcMode)
            {
                // In Sync.cpp
                processPioData(data);
            }
            else
            {
                // Echo back bytes, but not if HDLC mode ie enabled, then we want to echo back HDLC frames
                if (echoToSerial)
                {
                    printf("Received data: 0x%02X\r\n", data);
                }
            }

            // vTaskDelay(pdMS_TO_TICKS(10)); // 10ms delay
            if (cnt > 10)
            {
                return;
            }
        }
    }
}


#define MAX_LEN 1024           // Maximum input length
char inputBuffer[MAX_LEN + 1]; // Input buffer, with an extra byte for null-termination
int inputPos = 0;              // Position in the buffer

// ---- USB input, read in blocks ----
// The console used to call getchar_timeout_us(0) once per byte: one pass through the stdio
// driver list, the USB stdio mutex and the TinyUSB FIFO for every single byte. At 983000 bit/s
// the PC could feed only about 35 kbit/s of frame data through that (2026-11-09 one-way stream).
// Now one stdio_get_until() call takes everything the USB stack holds (up to the buffer size),
// and the bytes are handed out one by one from RAM.
static char usbIn[256];
static int usbInLen = 0;
static int usbInPos = 0;

// Measurements for STAT (see consoleInputStats)
static volatile uint32_t usbInReads = 0;       // stdio_get_until calls that returned data
static volatile uint32_t usbInBytes = 0;       // bytes those calls returned
static volatile uint32_t consoleWakeUsb = 0;   // console task woken by the USB "bytes arrived" callback
static volatile uint32_t consoleWakeTimeout = 0; // console task woken by its 10 ms ceiling instead

void consoleInputStats(uint32_t *reads, uint32_t *bytes, uint32_t *wakeUsb, uint32_t *wakeTimeout)
{
    *reads = usbInReads;
    *bytes = usbInBytes;
    *wakeUsb = consoleWakeUsb;
    *wakeTimeout = consoleWakeTimeout;
}

void consoleInputStatsClear()
{
    usbInReads = 0;
    usbInBytes = 0;
    consoleWakeUsb = 0;
    consoleWakeTimeout = 0;
}

// Next byte from the USB host, or PICO_ERROR_TIMEOUT when nothing is waiting
static int __not_in_flash_func(consoleNextChar)()
{
    if (usbInPos >= usbInLen)
    {
        int n = stdio_get_until(usbIn, (int)sizeof(usbIn), get_absolute_time()); // never waits
        if (n <= 0)
        {
            usbInLen = 0;
            usbInPos = 0;
            return PICO_ERROR_TIMEOUT;
        }
        usbInLen = n;
        usbInPos = 0;
        usbInReads++;
        usbInBytes += (uint32_t)n;
    }
    return (uint8_t)usbIn[usbInPos++];
}

// Function to handle serial input, including backspace and prompt display
void handleSerialInput()
{

    int rx_char = consoleNextChar(); // non-blocking
    while (rx_char != PICO_ERROR_TIMEOUT)
    {
        char c = (char)rx_char;
        toggleLED();


        if (systemState == DEBUG)
        {
            // Handle console keyboard input
            
            // Handle backspace/delete
            if ((c == '\b') || (c == '\x7F'))
            {
                if (inputPos > 0)
                {
                    inputPos--; // Move back in the buffer
                    inputBuffer[inputPos] = '\0';
                    printf("\b#\b"); // Erase the last character in the terminal
                }
            }
            // Handle CR or LF (Enter)
            else if (c == '\n' || c == '\r')
            {
                printf("\r\n");

                if (inputPos > 0)
                {
                    // Null-terminate the string and process it
                    inputBuffer[inputPos] = '\0';
                    processCommand(inputBuffer); // Process the command
                    inputPos = 0;                // Reset buffer for the next command
                }
                showPrompt(); // Show the prompt after processing the command
            }
            // Handle regular character input
            else
            {
                // Add character to buffer if within size limits
                if (inputPos < MAX_LEN)
                {
                    inputBuffer[inputPos++] = c; // Store the character
                    putchar(c);                  // Echo the character back
                }
                else
                {
                    // Buffer overflow, send error and reset buffer
                    printf("\nError: input exceeds maximum length of 1024 characters.\r\n");
                    inputPos = 0; // Clear the buffer
                    showPrompt(); // Show the prompt again
                }
            }
        }
        else if (systemState == PACKET_GEN)
        {
           // Check for 3 consecutive ESC to exit to FORWARDING mode
           static int escCount = 0;
           if (c == 0x1B) // ESC key
           {
               escCount++;
               if (escCount >= 3)
               {
                   printf("\r\n*** Switching to FORWARDING mode ***\r\n");
                   systemState = FORWARDING;
                   console_out_event(CONTROL_FRAME_EVENT_MODE_CHANGE, "FORWARDING");
                   escCount = 0;
                   return;
               }
               return; // Don't process ESC as a packet
           }
           else
           {
               escCount = 0; // Reset ESC count
           }
           
           //  Generate a packet per incoming char
           // Test a simple HDLC frame: 0x7E 0x01 0x2F 0x7E (SABM command)
           
           // Create HDLC frame based on the received character
           HDLC testFrame;
           uint8_t frameData[4];
           
           frameData[0] = 0x01;  // Address field
           frameData[1] = 0x3F;  // Control field (SABM command)
           frameData[2] = c;     // Data field - the received character
           frameData[3] = 0x00;  // Additional data or padding
           
           testFrame.CreateFrame(frameData, 4);
           
           // printf("Generated HDLC frame for char '%c' (0x%02X):\r\n",  (c >= 32 && c <= 126) ? c : '?', (uint8_t)c);
           // testFrame.DumpFrame();
           
           // Send as byte-stuffed frame to serial
           SendByteStuffed(&testFrame);
           
           //printf("Sent byte-stuffed frame\r\n");
        }
        else        
        { 
            // Forwarding state
            // Handle receive of byte stuffed HDLC frames and forward a complete frame to the HDLC interface
            processSerialByteStuffed(c);
        }


        // Try to read another char
        rx_char = consoleNextChar();
    }
}

// Function to update the prompt dynamically based on receiveBuffer and sendBuffer lengths
void showPrompt()
{
    if (systemState != DEBUG) return; // Only show prompt in DEBUG state

    int receiveLength = 0;
    int sendLength = 0;

    if (receiveBuffer == NULL)
    {
        printf("Receivebuffer is NULL\r\n");
    }
    else
    {
        receiveLength = receiveBuffer->Length(); // Access receiveBuffer length
        sendLength = sendBuffer->Length();       // Access sendBuffer length
    }

    int hdlcMode = 0;

    // Add a pre [n:n] ?
    if ((receiveLength > 0) || (sendLength > 0))
    {
        printf("[%d:%d]", receiveLength, sendLength);
    }

    if (hdlcMode)
    {
        putchar('@');
    }
    else
    {
        putchar('#');
    }

    if (echoToSerial)
    {
        putchar('>');
    }

    putchar(' ');
}

// The console task, so the USB receive callback can wake it
static TaskHandle_t consoleTaskHandle = NULL;

// Called by the Pico SDK's USB stdio driver from its low-priority USB interrupt after the USB
// stack has run and found received bytes waiting. Interrupt context: only the FromISR call here.
static void __not_in_flash_func(consoleCharsAvailable)(void *param)
{
    (void)param;
    if (consoleTaskHandle != NULL)
    {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(consoleTaskHandle, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

void tskConsole(void *pvParameters)
{
    // Enable CCP Bisync
    // hdlcReceiveFrame.EnableCCP(0x16);

    vTaskDelay(pdMS_TO_TICKS(100)); // 100ms delay
    // Wake this task as soon as USB has received bytes (see the end of the loop below)
    consoleTaskHandle = xTaskGetCurrentTaskHandle();
    stdio_set_chars_available_callback(consoleCharsAvailable, NULL);
    console_out_init();
    console_out_event(CONTROL_FRAME_EVENT_BOOT, "RC422Dev " RC422DEV_VERSION); // a frame in FORWARDING, text in DEBUG
    showPrompt();

    while (true)
    {
        // Read serial commands from serial port
        handleSerialInput();

        // Handle received data
        handleReceiveBufffer();

        // Periodic STAT frame to the host when it asked for one
        console_out_poll();

        // Sleep until USB has bytes for us, or at most 10 ms for the jobs above that are not
        // USB-driven (receive buffer, periodic STAT frame). This used to be a plain 10 ms delay:
        // the frame feed from the PC then came in bursts of one 64-byte USB buffer followed by
        // a 10 ms pause with the line clock stopped, which capped PC -> line traffic near
        // 14 / 22 / 26 kbit/s at 19200 / 38400 / 57600 bit/s (measured 2026-11-09). A wake-up
        // that arrives while the loop above is running is not lost: the notification count
        // stays set, so this take returns at once and the loop reads again.
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)) > 0)
        {
            consoleWakeUsb++;
        }
        else
        {
            consoleWakeTimeout++;
        }
    }
}

static void tskConsoleEchoSync(void *pvParameters)
{
    // Echo characters received from PIO to the console
#if false    
	while(true)
	{
     
		char c = sync_rx_program_getc(pio_rx, sm_rx);

		//putchar(c);
		printf("Received byte: 0x%02x\n", c);
	}
#endif
}

void initialize_console_task()
{
    // Initialize the LED pin as an output
    gpio_init(LED_PIN);              // Initialize the LED pin
    gpio_set_dir(LED_PIN, GPIO_OUT); // Set the LED pin as an output
    gpio_put(LED_PIN, ledState);     // Set the new state to the LED pin

    xTaskCreate(tskConsole,            // The function that implements the task.
                "Console",             // The text name assigned to the task - for debug only as it is not used by the kernel.
                4 * 1024,              // The size of the stack to allocate to the task.  configMINIMAL_STACK_SIZE =128
                NULL,                  // The parameter passed to the task - not used in this case.
                CONSOLE_TASK_PRIORITY, // The priority assigned to the task.
                NULL);

    /*
        while (true) {
            // Check if there's data in the RX FIFO
            if (!pio_sm_is_rx_fifo_empty(pio, sm_receive)) {
                uint8_t received_byte = pio_sm_get(pio, sm_receive) & 0xFF;  // Mask to 8 bits

                //send_byte(pio, sm_transmit,  received_byte); // echo back!

                printf("Received byte: 0x%02x\n", received_byte);


            }
        }
    */
}