#include "console.h"
#include "shared.h"
#include "hardware/gpio.h"
#include <pico/stdio.h>
#include "pico/stdlib.h"

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

// HDLC hdlcReceiveFrame;

void handleReceiveBufffer()
{
    int cnt = 0;

#if false
	if (receiveToSerialEnabled || loopbackEnabled || hdlcMode)
	{
		while (!receiveBuffer.isBufferEmpty())
		{
			cnt++;
			uint8_t data = receiveBuffer.readBuffer();
			if (loopbackEnabled)
			{
				sendBuffer.writeBuffer(data);  // Send received data to sender      
			}


			if (hdlcMode)
			{
				hdlcReceiveFrame.ReceiveByte(data);

        //Serial.printf("HDLC byte received 0x%02X. Len=%d, State=%d\r\n", data, hdlcReceiveFrame.Lenght(), hdlcReceiveFrame.GetState());

				if (hdlcReceiveFrame.GetState() == HDLC_STATE_FRAME_COMPLETE)
				{					
					
					int len = hdlcReceiveFrame.Lenght();
					if (len > 0)
					{
						Serial.printf("HDLC frame received, len=%d\r\n", len);
					}

					if ((receiveToSerialEnabled)&& (len>0))
					{
						for (int i = 0; i < len; i++)
						{
							uint8_t data = hdlcReceiveFrame.GetFrameByte(i);
							Serial.printf("0x%02X ", data);

							if ((i + 1) % 8 == 0)
							{
								Serial.println();
							}

						}
						Serial.println();
					}


					if (len > 0)
					{
						Serial.printf("HDLC frame size = %d bytes.\r\n", len);
					}
          			hdlcReceiveFrame.Clear(); // Ready for next
				}				
			}


			// Echo back bytes, but not if HDLC mode ie enabled, then we want to echo back HDLC frames
			if ((receiveToSerialEnabled) && (!hdlcMode))
			{
				if (data != 0)
				{
					Serial.printf("Received data: 0x%02X\r\n", data);
					Serial.flush();
				}
			}

			//vTaskDelay(pdMS_TO_TICKS(10)); // 10ms delay
			if (cnt > 10)
			{
				return;
			}
		}
	}
#endif
}

void handleEchoHDLC()
{
#if false
	if (hdlcMode)
	{


	}
#endif
}

#define MAX_LEN 1024           // Maximum input length
char inputBuffer[MAX_LEN + 1]; // Input buffer, with an extra byte for null-termination
int inputPos = 0;              // Position in the buffer

// Function to handle serial input, including backspace and prompt display
void handleSerialInput()
{

    int rx_char = getchar_timeout_us(0); // Non-blocking read from UART
    while (rx_char != PICO_ERROR_TIMEOUT)
    {
        char c = (char)rx_char;
        toggleLED();

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

        // Try to read another char
        rx_char = getchar_timeout_us(0); // Non-blocking read from UART
    }
}

// Function to update the prompt dynamically based on receiveBuffer and sendBuffer lengths
void showPrompt()
{
    int receiveLength = 0;
    int sendLength =0;

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

    if (receiveToSerialEnabled)
    {
        putchar('>');
    }

    putchar(' ');
}

void tskConsole(void *pvParameters)
{
    // Enable CCP Bisync
    // hdlcReceiveFrame.EnableCCP(0x16);
    
    vTaskDelay(pdMS_TO_TICKS(100)); // 100ms delay
    showPrompt();

    while (true)
    {
        // Read serial commands from serial port
        handleSerialInput();

        // Handle received data
        handleReceiveBufffer();

        // Handle echo HDLC packets received
        handleEchoHDLC();

        // Give some CPU back
        vTaskDelay(pdMS_TO_TICKS(10)); // 10ms delay
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

void initialize_console()
{
    // Initialize the LED pin as an output
    gpio_init(LED_PIN);              // Initialize the LED pin
    gpio_set_dir(LED_PIN, GPIO_OUT); // Set the LED pin as an output
    gpio_put(LED_PIN, ledState);     // Set the new state to the LED pin

    xTaskCreate(tskConsole,               // The function that implements the task.
                "Console",                // The text name assigned to the task - for debug only as it is not used by the kernel.
                4*1024,                   // The size of the stack to allocate to the task.  configMINIMAL_STACK_SIZE =128
                NULL,                     // The parameter passed to the task - not used in this case.
                CONSOLE_TASK_PRIORITY,    // The priority assigned to the task.
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