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
#define RX_PIN 16       // Right, down and 1 up

PIO pio_rx = pio1; // Use PIO instance 0
PIO pio_tx = pio0; // Use PIO instance 0

uint sm_rx = 0; // Use state machine 0 for receiving
uint sm_tx = 1; // Use state machine 1 for transmitting

// Define the task handles (will be set when rx and tx task is started)
TaskHandle_t handleTX = NULL;
TaskHandle_t handleRX = NULL;

#define RX_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define TX_TASK_PRIORITY (tskIDLE_PRIORITY + 2)

HDLC rxHDLC; // Receiver HDLC frame

//#define _reverse_

#ifdef _reverse_
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

#endif


// ******* DMA settings for RX **************
uint rx_dma_channel; // Define the DMA channel globally

#define BUFFER_SIZE 4000 //128
uint8_t rx_buffer[BUFFER_SIZE];
// uint32_t rx_buffer[BUFFER_SIZE];
volatile uint32_t last_position = 0;  // Last read position in the buffer
volatile bool is_buffer_full = false; // Flag to indicate buffer completion by DMA

//#define _receieve_with_rx_task_ 1
#define _use_pull_

void dma_irq_handler()
{
#ifdef _receieve_with_rx_task_
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	vTaskNotifyGiveFromISR(handleRX, &xHigherPriorityTaskWoken);
	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
#else

#ifdef _use_pull_
	// Indicate the buffer is full, ready for processing
	is_buffer_full = true;
#else
	// Receive all bytes ASAP
	while (!pio_sm_is_rx_fifo_empty(pio_rx, sm_rx))
	{
		// Write data from PIO to receiveBuffer
		uint8_t data = sync_rx_program_getc_blocking(pio_rx, sm_rx);
		receiveBuffer->writeBuffer(data);
	}

#endif

	// Re-Start DMA transfer
	// Reset the DMA write address to the beginning for continuous circular transfer
	dma_channel_set_write_addr(rx_dma_channel, rx_buffer, true);
#endif

	dma_hw->ints0 = 1u << rx_dma_channel; // Clear the interrupt flag
}

int setup_dma(PIO pio, uint sm)
{
	rx_dma_channel = dma_claim_unused_channel(true); // Set the global rx_dma_channel to point at a new unused channel
	if (rx_dma_channel == -1)
	{
		return PICO_ERROR_GENERIC;
	}

	// Configure the DMA channel
	dma_channel_config c = dma_channel_get_default_config(rx_dma_channel);

	// Configure for 8-bit transfer (modify if needed)
	channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
	channel_config_set_read_increment(&c, false); // Source is PIO FIFO, so no increment
	channel_config_set_write_increment(&c, true); // Destination buffer is in RAM, so increment

	channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false)); // Set DREQ for PIO

	dma_channel_configure(
		rx_dma_channel,
		&c,
		rx_buffer,	   // Destination buffer in memory
		&pio->rxf[sm], // Source address shifted by 3 bytes to capture only the MSB
		BUFFER_SIZE,   // Number of transfers
		false);		   // Start set to false for manual trigger

	// Set up interrupt handler for DMA completion
	dma_channel_set_irq0_enabled(rx_dma_channel, true);
	irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
	irq_set_enabled(DMA_IRQ_0, true);

	// Start DMA transfer
	dma_channel_set_write_addr(rx_dma_channel, rx_buffer, true);

	return PICO_OK;
}
// see also another option https://github.com/fivdi/pico-i2c-dma/blob/master/src/i2c_dma.c

void pull_from_dma_buffer()
{
	// Get the current DMA write position in the buffer for partial data
	uint32_t current_position = dma_channel_hw_addr(rx_dma_channel)->write_addr - (uint32_t)rx_buffer;

	// Determine the start and end points for processing
	uint32_t start = last_position;
	uint32_t end_position;

	if (is_buffer_full)
	{
		// If the buffer has wrapped, process up to BUFFER_SIZE and then from 0 to current_position
		end_position = BUFFER_SIZE;

		// Clear the flag after handling the wrap-around
		is_buffer_full = false;
	}
	else
	{
		// No wrap-around, just process up to current_position
		end_position = current_position;
	}

	for (uint32_t i = last_position; i < end_position; i++)
	{
		uint8_t data = rx_buffer[i];
#ifdef _reverse_
		receiveBuffer->writeBuffer(reverse_lookup[data]);
#else
		receiveBuffer->writeBuffer(data);
#endif
	}

	// Update last_position to current_position for the next poll
	if (end_position == BUFFER_SIZE)
	{
		last_position = 0; // Wrap around to start
	}
	else
	{
		last_position = end_position;
	}
}

void pio_irq_handler()
{
	// Check if the interrupt was triggered by data availability in the RX FIFO
	if (pio_interrupt_get(pio_rx, 0))
	{

		// Read data from the PIO RX FIFO
		// uint32_t data = pio_sm_get(pio_rx, sm_rx);

		// Process the received data
		// Write data from PIO to receiveBuffer
		while (!pio_sm_is_rx_fifo_empty(pio_rx, sm_rx))
		{
			// Write data from PIO to receiveBuffer
			uint8_t data = sync_rx_program_getc_blocking(pio_rx, sm_rx);
#ifdef _reverse_
			receiveBuffer->writeBuffer(reverse_lookup[data]);
#else
			receiveBuffer->writeBuffer(data);
#endif
		}

		// Clear the IRQ flag, and we are ready for more!
		pio_interrupt_clear(pio_rx, 0);
	}
}

void setup_pio_interrupt(PIO pio, uint sm)
{
	// Enable interrupt for RX FIFO "not empty" event
	pio_set_irq0_source_enabled(pio, (pio_interrupt_source_t)(pis_sm0_rx_fifo_not_empty + sm), true);

	// Enable IRQ for RX FIFO not empty
	if (pio == pio0)
	{
		irq_set_exclusive_handler(PIO0_IRQ_0, pio_irq_handler); // Using HW PIO0
		irq_set_enabled(PIO0_IRQ_0, true);
	}
	else
	{
		irq_set_exclusive_handler(PIO0_IRQ_1, pio_irq_handler); // Using HW PIO1
		irq_set_enabled(PIO0_IRQ_1, true);
	}
}


// Send a message to the RX pio to make is start looking for sync again
void reset_sync_detection(PIO pio, uint sm) {
    // Write any data to the TX FIFO to trigger a reset
    pio_sm_put(pio, sm, 0x1);  // Example data; actual value is irrelevant
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

static void tskSyncTX(void *pvParameters)
{
	// Remove compiler warning about unused parameter
	(void)pvParameters;

	// currentBaudRate = defaultBaudRate;

	printf("Initializing TX for %d bps\r\n", currentBaudRate);

	uint offset = pio_add_program(pio_tx, &sync_tx_program);
	sdlc_tx_program_init(pio_tx, sm_tx, offset, TX_PIN, TX_CLOCK_PIN, currentBaudRate);

	while (true)
	{
		while (sendBuffer->Length() > 0)
		{
			uint8_t data = sendBuffer->readBuffer();			
			sdlc_tx_program_write(pio_tx, sm_tx, data);
		}
	
		vTaskDelay(100);
	}	
}

// #define _use_pio_irq_
#define _loop_rx_

static void tskSyncRX(void *pvParameters)
{
	// Remove compiler warning about unused parameter
	(void)pvParameters;

	printf("Initializing RX for %d bps\r\n", currentBaudRate);

	// Load program
#if false // _load_normal_	
	//uint offset = pio_add_program(pio_rx, &sync_rx_program);
	uint offset = pio_add_program(pio_rx, &sync_rx_program);
	sdlc_rx_program_init(pio_rx, sm_rx, offset, RX_PIN, RX_CLOCK_PIN, currentBaudRate,true);
#else
	uint offset = pio_add_program(pio_rx, &sync_rx_8_hdlc_program);
	if (offset == -1)
	{
		printf("Unable to load PIO program\r\n");
	}
	//sdlc_rx_program_init(pio_rx, sm_rx, offset, RX_PIN, RX_CLOCK_PIN, currentBaudRate,false);	
	sdlc_rx_hdlc_program_init(pio_rx, sm_rx, offset, RX_PIN, RX_CLOCK_PIN);
#endif


#ifdef _loop_rx_
	int cnt_syn = 0;
	while (true)
	{
		uint8_t data = sync_rx_program_getc(pio_rx, sm_rx);
		// uint8_t data = sync_rx_program_getc_blocking(pio_rx, sm_rx);
		receiveBuffer->writeBuffer(data);

		if (data == 0x7E)
		{
			printf("SYN\r\n");
			cnt_syn++;
			if (cnt_syn > 1)
			{
				//sdlc_rx_hdlc_program_reset(pio_rx, sm_rx);
				pio_sm_exec(pio_rx, sm_rx, pio_encode_jmp(offset));

				cnt_syn = 0;
				printf("RESET SYNC\r\n");
				//reset_sync_detection(pio_rx, sm_rx);
			}
		}

	}
#endif

#ifdef _use_pio_irq_
	printf("Initializing RX for PIO IRQ\r\n");

	setup_pio_interrupt(pio_rx, sm_rx);
	while (true)
	{
		vTaskDelay(100);
	}
#else

	printf("Initializing RX for DMA with IRQ\r\n");

	// Setup DMA for receive
	if (setup_dma(pio_rx, sm_rx) != PICO_OK)
	{
		printf("DMA Setup failed for RX. RX without DMA enabled.\r\n");
		while (true)
		{
			// Write data from PIO to receiveBuffer
			uint8_t data = sync_rx_program_getc(pio_rx, sm_rx);
			// uint8_t data = sync_rx_program_getc_blocking(pio_rx, sm_rx);
#ifdef _reverse_
			receiveBuffer->writeBuffer(reverse_lookup[data]);
#else
			receiveBuffer->writeBuffer(data);
#endif
		}
	}

#ifdef _use_pull_
	while (true)
	{
		// Delay for the polling interval (1 ms)
		vTaskDelay(pdMS_TO_TICKS(1));
		{
			pull_from_dma_buffer();
		}
	}
#else
	while (true)
	{
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Wait for DMA notification

		// Process the received data in rx_buffer
		for (int i = 0; i < BUFFER_SIZE; i++)
		{
			uint8_t data = rx_buffer[i];
#ifdef _reverse_
			receiveBuffer->writeBuffer(reverse_lookup[data]);
#else
			receiveBuffer->writeBuffer(data);
#endif
		}

		// Restart DMA transfer if continuous data reception is needed
		// setup_dma(pio_rx, sm_rx);

		// Restart DMA transfer with same settings
		dma_channel_set_write_addr(rx_dma_channel, rx_buffer, true);
	}
#endif

#endif

#ifdef _cleanup_
	// Disable the state machine
	pio_sm_set_enabled(pio_rx, sm_rx, false);

	// Unload the program from the PIO instance

	pio_remove_program(pio_rx, &sync_rx_program, offset);
#endif
}

void initialize_sync_rx()
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