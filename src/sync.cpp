#include "hardware/pio.h"

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"
#include "pico/multicore.h"
#include <stdio.h>
#include <stdlib.h>
// #include "pico/stdlib.h"
// #include "hardware/pio.h"
// #include "hardware/pwm.h"  // Ensure this is included for PWM functions
#include "hardware/clocks.h" // Include for clock_get_hz function
#include "sdlc.pio.h"
#include "hdlc.h"
#include "shared.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#define TX_CLOCK_PIN 15 // Left, down
#define TX_PIN 14		// Left, down and 1 up

#define RX_CLOCK_PIN 17 // Right, down
#define RX_PIN 16		// Right, down and 1 up

PIO pio_rx = pio0; // Use PIO instance 0
PIO pio_tx = pio1; // Use PIO instance 0

int pio_irq_index = 0; // PIO interrupt index (0 or 1 for PIO interrupt source)
uint rx_irq_num;	   // NVIC IRQ number (PIO0_IRQ_0, etc.)

uint sm_rx = 0; // Use state machine 0 for receiving
uint sm_tx = 1; // Use state machine 1 for transmitting

uint offset_rx = -1; // Offset for RX code loaded into state machine
uint offset_tx = -1; // Offset for TX code loaded into state machine

// Define the task handles (will be set when rx and tx task is started)
TaskHandle_t handleTX = NULL;
TaskHandle_t handleRX = NULL;

#define RX_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define TX_TASK_PRIORITY (tskIDLE_PRIORITY + 2)

#define MAX_FRAME_QUEUE_SIZE 10
QueueHandle_t hdlcQueue = nullptr;

HDLC *rxHDLC = nullptr; // Define the receiveBuffer pointer

// #define _reverse_

//#ifdef _reverse_
const uint8_t reverse_lookup[256] = {
	0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0, 0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
	0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8, 0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
	0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4, 0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
	0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC, 0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
	0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2, 0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
	0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA, 0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
	0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
	0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE, 0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
	0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1, 0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
	0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9, 0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
	0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5, 0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
	0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED, 0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
	0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3, 0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
	0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB, 0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
	0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7, 0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
	0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF, 0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF};

//#endif

// Send current (complete) HDLC frame to a queue, allocate a new frame 
// RX Task will process complete frames from queue
void queueHDLCFrame()
{

	// Queue complete frame
	if (xQueueSendFromISR(hdlcQueue, &rxHDLC, NULL) != pdPASS)
	{
		// Handle error: queue is full
		printf("Error: HDLC Frame queue is full\r\n");
		rxHDLC->DumpFrame();
		delete rxHDLC;
		rxHDLC = NULL;
	}

	// Allocate new empty HDLC frame object
	rxHDLC = new HDLC();
}


void inspectCurrentHdlcFrame()
{
	if (rxHDLC != nullptr)
	{
		rxHDLC->DumpFrame();
	}
}

int cnt_syn = 0;
void processPioData(uint8_t data)
{
	//printf("PIO RX[0x%02X]\r\n ", data);

	if (rxHDLC != NULL)
	{

		// Dont use receivebyte as we need to know exectly when the full HDLC frame is received, and it doesnt neccesarily mean its one a byte boundary
		// rxHDLC->ReceiveByte(data);

		for (int i = 0; i < 8; i++)
		{
			if (data & 1)
			{
				//printf("I");

				rxHDLC->ReceiveBit(1);
			}
			else
			{
				//printf("O");
				rxHDLC->ReceiveBit(0);
			}

			HDLCReceiveState rxState = rxHDLC->GetReceiveState();

			// printf("[1] FRAMESTATE %d\r\n", rxState);

			if (rxState == HDLC_STATE_FRAME_COMPLETE)
			{
				printf("Frame complete - queueing\r\n");
				queueHDLCFrame();
			}

			data = data >> 1; // shift right, so we can get next bit
		}
	}

/*
	if ((data == 0x7E) || (data == 0x3F)) // data 1 bit moved to the right after a bitstuffer removed
	{
		printf("SYN\r\n");
		cnt_syn++;
		if (cnt_syn > 1)
		{
			// sdlc_rx_hdlc_program_reset(pio_rx, sm_rx);
			pio_sm_exec(pio_rx, sm_rx, pio_encode_jmp(offset_rx));

			cnt_syn = 0;
			printf("RESET SYNC\r\n");
			// reset_sync_detection(pio_rx, sm_rx);
		}
	}
*/	
}

void printHDLCStat()
{
	if (rxHDLC != nullptr)
	{
		rxHDLC->DumpFrame();
	}
}

void pio_irq_handler()
{	
	// Process the received data
	// Write data from PIO to receiveBuffer or HDLC frame
	while (!pio_sm_is_rx_fifo_empty(pio_rx, sm_rx))
	{
		uint8_t data = sync_rx_program_getc(pio_rx, sm_rx);

		if (hdlcMode)
		{
			processPioData(data);
		}
		else
		{
			receiveBuffer->writeBuffer(data);
		}
	}

	// Clear the NVIC interrupt flag if necessary
	irq_clear(rx_irq_num);
}

void setup_pio_interrupt(PIO pio, uint sm)
{
	// Determine the NVIC IRQ number based on the PIO instance
	if (pio == pio0)
	{
		rx_irq_num = PIO0_IRQ_0; // NVIC interrupt for PIO0
	}
	else
	{
		rx_irq_num = PIO1_IRQ_0; // NVIC interrupt for PIO1
	}

	// Enable the RX FIFO not empty interrupt for the specified state machine
	pio_set_irq0_source_enabled(pio, (pio_interrupt_source_t)(PIO_INTR_SM0_RXNEMPTY_LSB + sm), true);

	// Set up the interrupt handler for the specified PIO IRQ
	irq_set_exclusive_handler(rx_irq_num, pio_irq_handler);
	irq_set_enabled(rx_irq_num, true);
}

void send_byte(PIO pio, uint sm, uint8_t byte)
{
	// Wait until there is space in the FIFO
	while (pio_sm_is_tx_fifo_full(pio, sm))
	{
		// Optionally, add a timeout here if needed
		vTaskDelay(1); // Small delay to avoid a tight loop if needed
	}

	// Now we know there is space in the FIFO(blocking put)
	pio_sm_put(pio, sm, byte);
}


void sendHdlcByte_org(uint8_t data, bool isFlag)
{
	// printf("TX[0x%02X]\r\n ", data);
	//  sdlc_tx_program_write(pio_tx, sm_tx, data);	

	//data = reverse_lookup[data];

	// Create bitstuffing for send
	uint16_t tx_data = 0; // Transmit data with potential stuffed bits
	uint8_t ones = 0;	  // Count of consecutive ones
	uint8_t sendbits = 0; // Bits to send after stuffing (starts counting at 0)
	
	if (isFlag)
	{
		tx_data = data;
		sendbits = 8;
	}
	else
	{
		for (int i = 0; i < 8; i++)
		{
			uint8_t bit = (data & 0x80) ? 1 : 0; // Get the MSB bit of data as either 0 or 1
			data <<= 1;							 // Shift data left to process the next bit

			// Add the current bit to tx_data in the correct order
			tx_data |= (bit << sendbits);
			sendbits++;

			if (bit)
			{
				ones++; // Increment consecutive ones counter
			}
			else
			{
				ones = 0; // Reset on a 0 bit
			}

			// Insert a stuffed zero after five consecutive ones
			if (ones == 5)
			{
				printf("---  STUFF ---");
				tx_data |= (0 << sendbits); // Explicitly set the stuffed zero bit
				sendbits++;					// Increment the bit count to reflect the stuffed bit
				ones = 0;					// Reset consecutive ones counter
			}
		}
	}

	//printf("TX[0x%04X] cnt=%d\r\n ", tx_data, sendbits - 1);
	sdlc_tx_program_write_cnt(pio_tx, sm_tx, tx_data, sendbits - 1);
}

void print_binary(uint8_t n) {
    for (int i = 7; i >= 0; i--) {
        printf("%d", (n >> i) & 1);
    }    
}



// Number of consecutive ones (must be counted across sent bytes)
uint8_t ones = 0;	  

void sendHdlcByte(uint8_t data, bool isFlag)
{
	/*
	printf("TX[0x%02X] %d [", data,isFlag);
	print_binary(data);	
	printf("]  Sending[");
	*/
	//  sdlc_tx_program_write(pio_tx, sm_tx, data);	
	// Create bitstuffing for send
	uint16_t tx_data = 0; // Transmit data with potential stuffed bits
	
	uint8_t sendbits = 0; // Bits to send after stuffing (starts counting at 0)
	
	if (isFlag)
	{
		tx_data = data;
		sendbits = 8;
	}
	else
	{
		for (int i = 0; i < 8; i++)
		{
			uint8_t bit = (data & 0x01) ? 1 : 0; // Get the MSB bit of data as either 0 or 1
			data >>= 1;							 // Shift data left to process the next bit

			// Add the current bit to tx_data in the correct order
			tx_data |= (bit << sendbits);
			sendbits++;

			if (bit)
			{
				//printf("1");
				ones++; // Increment consecutive ones counter
			}
			else
			{
				//printf("0");
				ones = 0; // Reset on a 0 bit
			}

			// Insert a stuffed zero after five consecutive ones
			if (ones == 5)
			{
				//printf("---  STUFF at %d ---",i);
				//printf("S");
				tx_data |= (0 << sendbits); // Explicitly set the stuffed zero bit
				sendbits++;					// Increment the bit count to reflect the stuffed bit
				ones = 0;					// Reset consecutive ones counter
			}
		}
	}

	//printf("]\r\n");

	//printf("TX[0x%04X] cnt=%d\r\n ", tx_data, sendbits - 1);
	sdlc_tx_program_write_cnt(pio_tx, sm_tx, tx_data, sendbits - 1);
}

static void tskSyncTX(void *pvParameters)
{
	// Remove compiler warning about unused parameter
	(void)pvParameters;

	// currentBaudRate = defaultBaudRate;

	printf("Initializing TX for %d bps\r\n", currentBaudRate);

	// offset_tx = pio_add_program(pio_tx, &sync_tx_program);
	offset_tx = pio_add_program(pio_tx, &sync_tx_with_count_program);
	sdlc_tx_program_init(pio_tx, sm_tx, offset_tx, TX_PIN, TX_CLOCK_PIN, currentBaudRate);


	// Loop and read data from the ringbuffer 'sendBuffer' and send it
	while (true)
	{
		while (sendBuffer->Length() > 0)
		{
			uint8_t data = sendBuffer->readBuffer();
			bool isFlag = false;
			if (data == 0x7E) // syn char
			{
				isFlag = true;
			}
			sendHdlcByte(data, isFlag);
		}
	}

	vTaskDelay(1);
}

static void tskSyncRX(void *pvParameters)
{
	// Remove compiler warning about unused parameter
	(void)pvParameters;

	printf("Initializing RX for %d bps\r\n", currentBaudRate);

	// allocate HDLC frame for receive
	rxHDLC = new HDLC();

	hdlcQueue = xQueueCreate(MAX_FRAME_QUEUE_SIZE, sizeof(HDLC *));

	// Load program
#if false // _load_normal_	
	//uint offset = pio_add_program(pio_rx, &sync_rx_program);
	uint offset = pio_add_program(pio_rx, &sync_rx_program);
	sdlc_rx_program_init(pio_rx, sm_rx, offset, RX_PIN, RX_CLOCK_PIN, currentBaudRate,true);
#else
	offset_rx = pio_add_program(pio_rx, &sync_rx_8_hdlc_program);
	if (offset_rx == -1)
	{
		printf("Unable to load PIO program\r\n");
	}
	// sdlc_rx_program_init(pio_rx, sm_rx, offset, RX_PIN, RX_CLOCK_PIN, currentBaudRate,false);
	sdlc_rx_hdlc_program_init(pio_rx, sm_rx, offset_rx, RX_PIN, RX_CLOCK_PIN);
#endif

#if false
	while(true)
	{
		// Process the received data
		// Write data from PIO to receiveBuffer
		while (!pio_sm_is_rx_fifo_empty(pio_rx, sm_rx))
		{
			uint8_t data = sync_rx_program_getc(pio_rx, sm_rx);
			processPioData(data);
		}

		vTaskDelay(10);
	}
#endif

	printf("Initializing RX for PIO IRQ\r\n");
	setup_pio_interrupt(pio_rx, sm_rx);

	// Waiting for HDLC frames to process
	while (true)
	{
		HDLC *frame;

		// Process the HDLC frame here
		while (xQueueReceive(hdlcQueue, &frame, portMAX_DELAY) == pdPASS)
		{
			printf("\r\n");
			printf("===> Got HDLC frame... \r\n");

			// frame->process();  // Assuming you have a method to handle the frame
			if (echoToSerial)
			{
				printf("-------------------------------------------------\r\n");
				frame->DumpFrame();
				printf("-------------------------------------------------\r\n");
			}

			// Delete the frame after processing
			delete frame;
		}

		vTaskDelay(100);
	}

#ifdef _cleanup_
	// Disable the state machine
	pio_sm_set_enabled(pio_rx, sm_rx, false);

	// Unload the program from the PIO instance
	pio_remove_program(pio_rx, &sync_rx_8_hdlc_program, offset);
#endif
}

void initialize_sync_tasks()
{

	xTaskCreate(tskSyncTX,		  // The function that implements the task.
				"SyncTX",		  // The text name assigned to the task - for debug only as it is not used by the kernel.
				1 * 1024,		  // The size of the stack to allocate to the task.
				NULL,			  // The parameter passed to the task - not used in this case.
				TX_TASK_PRIORITY, // The priority assigned to the task.
				&handleTX);

	xTaskCreate(tskSyncRX,		  // The function that implements the task.
				"SyncRX",		  // The text name assigned to the task - for debug only as it is not used by the kernel.
				1 * 1024,		  // The size of the stack to allocate to the task.
				NULL,			  // The parameter passed to the task - not used in this case.
				RX_TASK_PRIORITY, // The priority assigned to the task.
				&handleRX);

	// Pin Tasks
#if (configNUMBER_OF_CORES > 1)
	vTaskCoreAffinitySet(handleTX, (1 << 1)); // Core 0
	vTaskCoreAffinitySet(handleRX, (1 << 0)); // Core 1
#endif
}