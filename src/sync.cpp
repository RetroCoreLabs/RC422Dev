#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <stdlib.h>
//#include "pico/stdlib.h"
//#include "hardware/pio.h"
//#include "hardware/pwm.h"  // Ensure this is included for PWM functions
#include "hardware/clocks.h"   // Include for clock_get_hz function
#include "sdlc.pio.h"
#include "hdlc.h"
#include "shared.h"

#define TX_CLOCK_PIN 15 // GP15
#define TX_PIN 14 // GP14


#define RX_CLOCK_PIN 17
#define RX_PIN 16 

PIO pio_rx = pio0;          // Use PIO instance 0
PIO pio_tx = pio0;          // Use PIO instance 0

uint sm_rx = 0;    // Use state machine 0 for receiving
uint sm_tx = 1;    // Use state machine 1 for transmitting


#define RX_TASK_PRIORITY ( tskIDLE_PRIORITY + 2 )
#define TX_TASK_PRIORITY ( tskIDLE_PRIORITY + 2 )


HDLC rxHDLC; // Receiver HDLC frame

#if _irq_
// IRQ called when the pio fifo is not empty, i.e. there are some characters on the uart
// This needs to run as quickly as possible or else you will lose characters (in particular don't printf!)
static void pio_irq_func(void) {
	while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
		char c = uart_rx_program_getc(pio, sm);
		if (!queue_try_add(&fifo, &c)) {
			panic("fifo full");
		}
	}
	// Tell the async worker that there are some characters waiting for us
	async_context_set_work_pending(&async_context.core, &worker);
}
#endif



void send_byte(PIO pio, uint sm, uint8_t byte) {
	// Wait until there is space in the FIFO
	while (pio_sm_is_tx_fifo_full(pio, sm)) {
		// Optionally, add a timeout here if needed
		vTaskDelay(1);  // Small delay to avoid a tight loop if needed
	}

	// Now we know there is space in the FIFO(blocking put)
	pio_sm_put(pio, sm, byte);
}


static void tskSyncTX( void *pvParameters )
{
	// Remove compiler warning about unused parameter
	( void ) pvParameters;

	
    //currentBaudRate = defaultBaudRate;

	printf("Initializing TX for %d bps\r\n", currentBaudRate);

	uint offset = pio_add_program(pio_tx, &sync_tx_program);
	sdlc_tx_program_init(pio_tx, sm_tx, offset, TX_PIN, TX_CLOCK_PIN, currentBaudRate);

	while (true)
	{
		while( sendBuffer->Length() >0)
		{			
			uint8_t data = sendBuffer->readBuffer();		
			sdlc_tx_program_write(pio_tx, sm_tx, data);			
		}				
		vTaskDelay( 100 );
	}
}

static void tskSyncRX( void *pvParameters )
{
	// Remove compiler warning about unused parameter
	( void ) pvParameters;

	printf("Initializing TX for %d bps\r\n", currentBaudRate);

	// Load program
	uint offset = pio_add_program(pio_rx, &sync_rx_program);
	sdlc_rx_program_init(pio_rx, sm_rx, offset, RX_PIN, RX_CLOCK_PIN,currentBaudRate);

	int cnt = 0;
	// Echo characters received from PIO to the console
	while(true)
	{     
		//char c = sync_rx_program_getc(pio_rx, sm_rx);
		uint8_t data = sync_rx_program_getc_blocking(pio_rx, sm_rx);
		receiveBuffer->writeBuffer(data);		
	}

	// Disable the state machine
	pio_sm_set_enabled(pio_tx, sm_tx, false);

	// Unload the program from the PIO instance
	pio_remove_program(pio_tx, &sync_tx_program, offset);
}


void initialize_sync_rx() 
{

    xTaskCreate( tskSyncTX,				        // The function that implements the task.
                "SyncTX", 			        // The text name assigned to the task - for debug only as it is not used by the kernel.
                1*1024, 				  	// The size of the stack to allocate to the task.
                NULL, 						    // The parameter passed to the task - not used in this case. 
                TX_TASK_PRIORITY, 	        // The priority assigned to the task. 
                NULL );		


    xTaskCreate( tskSyncRX,				        // The function that implements the task.
                "SyncRX", 			        // The text name assigned to the task - for debug only as it is not used by the kernel.
                1*1024,				   	// The size of the stack to allocate to the task.
                NULL, 						    // The parameter passed to the task - not used in this case. 
                TX_TASK_PRIORITY, 	        // The priority assigned to the task. 
                NULL );		


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