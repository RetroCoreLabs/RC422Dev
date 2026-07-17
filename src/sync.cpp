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
#include "sync.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include <pico/stdio.h>

#define TX_CLOCK_PIN 15 // Left, down
#define TX_PIN 14		// Left, down and 1 up

#define RX_CLOCK_PIN 17 // Right, down and 1 up
#define RX_PIN 16		// Right, down

PIO pio_rx = pio0; // Use PIO instance 0
PIO pio_tx = pio0; // Use PIO instance 0

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

void reset_rx_statemachine()
{
	return;
	// Disable the state machine
	pio_sm_set_enabled(pio_rx, sm_rx, false);

	// Clear the FIFO (optional, ensures there’s no leftover data from the previous frame)
	pio_sm_clear_fifos(pio_rx, sm_rx);

	// Re-enable the state machine, which resets it to start from offset_rx
	pio_sm_restart(pio_rx, sm_rx);
	pio_sm_set_enabled(pio_rx, sm_rx, true);
}

int cnt_syn = 0;
int cnt_no_sync = 0;
void processPioData(uint8_t data)
{

	// printf("PIO RX[0x%02X]\r\n ", data); // addint print here will make us loose bits - its time sensitive
#if false
	HDLCReceiveState rxState = rxHDLC->GetReceiveState();

	if (rxState == HDLC_STATE_IDLE)
	{
		if (data != HDLC::HDLC_FLAG)
		{
			reset_rx_statemachine();
			return;
		}
	}
	else if (rxState == HDLC_STATE_FLAG_DETECTED)
	{
		// printf("PIO RX[0x%02X] S[%d]\r\n ", data, rxState);
	}


	if ((rxState == HDLC_STATE_IDLE) || (HDLC_STATE_FLAG_DETECTED))
	{
		cnt_no_sync++;
	}
	else
	{
		cnt_no_sync = 0;
	}

	if (cnt_no_sync > 3)
	{
		// printf("lost sync)\r\n");
		//  Reset RX back to SYN hunt
		reset_rx_statemachine();
		// pio_sm_exec(pio_rx, sm_rx, pio_encode_jmp(offset_rx));
		cnt_no_sync = 0;
		return;
	}
#endif

	if (rxHDLC != NULL)
	{

		// Dont use receivebyte as we need to know exectly when the full HDLC frame is received, and it doesnt neccesarily mean its one a byte boundary
		// rxHDLC->ReceiveByte(data);

		for (int i = 0; i < 8; i++)
		{
			if (data & 1)
			{
				// printf("I");

				rxHDLC->ReceiveBit(1);
			}
			else
			{
				// printf("O");
				rxHDLC->ReceiveBit(0);
			}

			HDLCReceiveState rxState = rxHDLC->GetReceiveState();

			// printf("[1] FRAMESTATE %d\r\n", rxState);

			if (rxState == HDLC_STATE_FRAME_COMPLETE)
			{
				reset_rx_statemachine();
				// printf("Frame complete - queueing\r\n");
				queueHDLCFrame();
			}

			data = data >> 1; // shift right, so we can get next bit
		}
	}
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

void print_binary(uint8_t n)
{
	for (int i = 7; i >= 0; i--)
	{
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
				// printf("1");
				ones++; // Increment consecutive ones counter
			}
			else
			{
				// printf("0");
				ones = 0; // Reset on a 0 bit
			}

			// Insert a stuffed zero after five consecutive ones
			if (ones == 5)
			{
				// printf("---  STUFF at %d ---",i);
				// printf("S");
				tx_data |= (0 << sendbits); // Explicitly set the stuffed zero bit
				sendbits++;					// Increment the bit count to reflect the stuffed bit
				ones = 0;					// Reset consecutive ones counter
			}
		}
	}

	// printf("]\r\n");

	// printf("TX[0x%04X] cnt=%d\r\n ", tx_data, sendbits - 1);
	sdlc_tx_program_write_cnt(pio_tx, sm_tx, tx_data, sendbits - 1);
}

void SendByteStuffed(HDLC *frame)
{
	// Send byte-stuffed frame to serial port according to RFC 1662
	if (frame == nullptr)
		return;

	// Send opening flag
	putchar_raw(HDLC::HDLC_FLAG);

	int len = frame->Length();
	for (int i = 0; i < len; i++)
	{
		uint8_t data = frame->GetFrameByte(i);
		
		// Check if byte needs to be escaped according to RFC 1662
		if (data == HDLC::HDLC_FLAG)  // Flag sequence
		{
			putchar_raw(HDLC::HDLC_ESCAPE);
			putchar_raw(HDLC::HDLC_FLAG_ESC);
		}
		else if (data == HDLC::HDLC_ESCAPE)  // Escape sequence
		{
			putchar_raw(HDLC::HDLC_ESCAPE);
			putchar_raw(HDLC::HDLC_ESCAPE_ESC);
		}
		else if (data < 0x20)  // Control characters (optional in RFC 1662)
		{
			putchar_raw(HDLC::HDLC_ESCAPE);
			putchar_raw(data ^ HDLC::HDLC_ESCAPE_MASK);
		}
		else
		{
			putchar_raw(data);  // Send as-is
		}
	}

	// Send closing flag
	putchar_raw(HDLC::HDLC_FLAG);

	stdio_flush();  // Ensure all data is transmitted
}

// Byte-stuffed frame reception state machine
enum ByteStuffedState
{
	BS_IDLE,				// Waiting for frame start
	BS_RECEIVING_DATA,		// Receiving frame data
	BS_ESCAPE_RECEIVED		// Previous byte was escape character
};

ByteStuffedState byteStuffedState = BS_IDLE;
uint8_t byteStuffedBuffer[256];  // Buffer for incoming byte-stuffed frame
int byteStuffedPos = 0;          // Current position in buffer
int escapeCount = 0;             // Count of consecutive ESC characters
int receivedFrameCount = 0;      // Count of received frames in current session

void resetByteStuffedReceiver()
{
	byteStuffedState = BS_IDLE;
	byteStuffedPos = 0;
	escapeCount = 0;
}

void processSerialByteStuffed(uint8_t data)
{
	// Check for 3 consecutive ESC characters to switch to DEBUG mode
	// Only check when no frames have been received and not currently receiving a frame
	if (receivedFrameCount == 0 && byteStuffedState == BS_IDLE)
	{
		if (data == 0x1B)  // ESC keyboard key
		{
			escapeCount++;
			if (escapeCount >= 3)
			{
				// Switch to DEBUG mode
				systemState = DEBUG;
				printf("\r\n*** Switching to DEBUG mode ***\r\n");
				resetByteStuffedReceiver();				
				return;
			}
		}
		else
		{
			escapeCount = 0; // Reset count if non-ESC character received
		}
	}

	// Process byte-stuffed frame according to RFC 1662
	switch (byteStuffedState)
	{
		case BS_IDLE:
			if (data == HDLC::HDLC_FLAG)
			{
				// Start of frame detected
				byteStuffedState = BS_RECEIVING_DATA;
				byteStuffedPos = 0;
				escapeCount = 0; // Reset ESC count when frame starts
			}
			break;

		case BS_RECEIVING_DATA:
			if (data == HDLC::HDLC_FLAG)
			{
				// End of frame detected
				if (byteStuffedPos > 0)
				{
					// Create HDLC frame from received data and send to HDLC interface
					HDLC newFrame;
					newFrame.CreateFrameFromByteStuffed(byteStuffedBuffer, byteStuffedPos);					
					SendHDLCFrame(&newFrame);
					
					receivedFrameCount++;
					//printf("Forwarded frame %d, length %d bytes\r\n", receivedFrameCount, byteStuffedPos);
					
				}
				byteStuffedState = BS_IDLE;
				byteStuffedPos = 0;
			}
			else if (data == HDLC::HDLC_ESCAPE)
			{
				byteStuffedState = BS_ESCAPE_RECEIVED;
			}
			else
			{
				// Regular data byte
				if (byteStuffedPos < sizeof(byteStuffedBuffer) - 1)
				{
					byteStuffedBuffer[byteStuffedPos++] = data;
				}
				else
				{
					// Buffer overflow - reset and start over
					printf("Buffer overflow in byte-stuffed reception\r\n");
					resetByteStuffedReceiver();
				}
			}
			break;

		case BS_ESCAPE_RECEIVED:
			// Previous byte was escape, decode the escaped byte
			// XOR with 0x20 to get original value
				if (byteStuffedPos < sizeof(byteStuffedBuffer) - 1)
				{
					byteStuffedBuffer[byteStuffedPos++] = data ^ 0x20;
				}
			
			byteStuffedState = BS_RECEIVING_DATA;
			
			// Check for buffer overflow
			if (byteStuffedPos >= sizeof(byteStuffedBuffer) - 1)
			{
				// printf("Buffer overflow in byte-stuffed reception\r\n");
				resetByteStuffedReceiver();
			}
			break;

	}
}

void SendHDLCFrame(HDLC *frame)
{
	// for (int x = 0; x < 1000; x++)
	{
		sendHdlcByte(HDLC::HDLC_FLAG, true);
		int len = frame->Length();
		for (int i = 0; i < len; i++)
		{
			uint8_t data = frame->GetFrameByte(i);
			bool isFlag = false;
			if (data == HDLC::HDLC_FLAG) // syn char
			{
				isFlag = true;
			}

			sendHdlcByte(data, isFlag);
			// sendBuffer->writeBuffer(data & 0xFF); // Send to buffer
		}

		sentStats.okFrames++;
		sentStats.totalFrames++;
		sentStats.bytes += len;
	}
	// Finish with one extra flag to push all bits through
	sendHdlcByte(HDLC::HDLC_FLAG, true);
}

static void tskSyncTX(void *pvParameters)
{
	// Remove compiler warning about unused parameter
	(void)pvParameters;

	// currentBaudRate = defaultBaudRate;

	printf("Initializing TX for %d bps\r\n", currentBaudRate);

	// offset_tx = pio_add_program(pio_tx, &sync_tx_program);
	// sdlc_tx_program_init(pio_tx, sm_tx, offset_tx, TX_PIN, TX_CLOCK_PIN, currentBaudRate);

	offset_tx = pio_add_program(pio_tx, &sync_tx_with_count_program);
	if (offset_tx < 0)
	{
		panic("Unable to load TX PIO program");
	}

	printf("TX PIO code loaded at offset %d\r\n", offset_tx);

	sdlc_tx_with_count_program_init(pio_tx, sm_tx, offset_tx, TX_PIN, TX_CLOCK_PIN, currentBaudRate);

	// Loop and read data from the ringbuffer 'sendBuffer' and send it
	while (true)
	{
		while (sendBuffer->Length() > 0)
		{
			uint8_t data = sendBuffer->readBuffer();
			bool isFlag = false;
			if (data == HDLC::HDLC_FLAG) // syn char
			{
				isFlag = true;
			}
			sendHdlcByte(data, isFlag);
		}

		vTaskDelay(10);
	}

	while (true)
	{
		vTaskDelay(100);
	}
}

void processFrame(HDLC *frame)
{

	if (frame == nullptr)
		return;

	/*
		Summary of the Control Messages

			Control Byte	Frame Type		P/F Bit	Frame Details
			0x3F			U-frame			1		SABM - Set Asynchronous Balanced Mode, initiating a balanced mode session
			0x73			S-frame			1		RR - Receive Ready, N(R) = 3, acknowledges frames up to N(R) = 3
			0x91			S-frame			1		RR - Receive Ready, N(R) = 4, acknowledges frames up to N(R) = 4
	*/

	int len = frame->Length();
	if (len > 2)
	{
		printf("+++++++++++++++++++++++++++++++++++++++++++++++++\r\n");

		hdlc_control_field_t ctrl = frame->GetControlField();
		frame->DumpControlField(ctrl);

		uint8_t d0 = frame->GetFrameByte(0);
		uint8_t d1 = frame->GetFrameByte(1);

		printf("ProcessFrame, Len=%d, Address[0x%02X] Control[0x%02X]\r\n", len, d0, d1);
		uint8_t dataArray[100];
		uint8_t poll_final = 1;

		if (d0 == 0)
		{
			// Address = 0 =>  No-Station Address
			// This address serves a specific purpose: testing scenarios where no data station is intended to react to or respond to a transmitted frame.
			printf("No response for No-Station Address\r\n");
		}

		if (ctrl.frame_type == HDLC_FRAME_TYPE_U)
		{
			dataArray[0] = d0;

			HDLC newframe;
			switch (ctrl.u_type) // hdlc_u_frame_type_t
			{
			case HDLC_U_MSG_CMD_SABM:

				// TODO: Use 3 bit sequence number
				dataArray[1] = frame->CreateControlU(HDLC_U_MSG_RSP_UA, poll_final);
				dataArray[2] = 0x00;
				dataArray[3] = 0x67; // MY Machine ID
				newframe.CreateFrame(dataArray, 4);
				SendHDLCFrame(&newframe);

				printf("Sent response message:\r\n");
				newframe.DumpFrame();
				break;

			case HDLC_U_MSG_CMD_SNRM:
			case HDLC_U_MSG_CMD_SARM:
				// TODO: Use 3 bit sequence number
				dataArray[1] = frame->CreateControlU(HDLC_U_MSG_RSP_UA, poll_final);
				newframe.CreateFrame(dataArray, 2);
				SendHDLCFrame(&newframe);

				printf("Sent response message:\r\n");
				newframe.DumpFrame();
				break;
			case HDLC_U_MSG_CMD_SABME:
			case HDLC_U_MSG_CMD_SNRME:
			case HDLC_U_MSG_CMD_SARME:

				// TODO: Use 7 bit sequence number
				dataArray[1] = frame->CreateControlU(HDLC_U_MSG_RSP_UA, poll_final);
				newframe.CreateFrame(dataArray, 2);
				SendHDLCFrame(&newframe);

				printf("Sent response message:\r\n");
				newframe.DumpFrame();
				break;

			default:
				printf("No response defined for U MSG type %s (%s)\r\n", ctrl.txt_msg_type, ctrl.txt_msg_description);
				break;
			}
		}
		else if (ctrl.frame_type == HDLC_FRAME_TYPE_S)
		{
			dataArray[0] = d0;

			HDLC newframe;
			switch (ctrl.s_type) // hdlc_u_frame_type_t
			{
			case HDLC_S_FRAME_RR: // Receive Ready
								  // Control: 0x91  (RR frame with N(R) = 4, Final bit set)

				dataArray[1] = frame->CreateControlS(ctrl.s_type, ctrl.nr, poll_final);
				dataArray[2] = 0x00;
				dataArray[3] = 0x67; // MY Machine ID
				newframe.CreateFrame(dataArray, 4);
				SendHDLCFrame(&newframe);

				printf("Sent response message:\r\n");
				newframe.DumpFrame();

			case HDLC_S_FRAME_RNR:	// Receive Not Ready
			case HDLC_S_FRAME_REJ:	// Reject
			case HDLC_S_FRAME_SREJ: // Selective Reject
			default:
				printf("No response defined for S MSG type %s (%s)\r\n", ctrl.txt_msg_type, ctrl.txt_msg_description);
				break;
			}
		}
		else if (ctrl.frame_type == HDLC_FRAME_TYPE_I)
		{
			printf("No response defined for I MSG type %s (%s)\r\n", ctrl.txt_msg_type, ctrl.txt_msg_description);
		}

		printf("+++++++++++++++++++++++++++++++++++++++++++++++++\r\n");
	}
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
	if (offset_rx < 0)
	{
		panic("Unable to load RX PIO program");
	}

	printf("RX PIO code loaded at offset %d\r\n", offset_rx);
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
			receivedStats.totalFrames++;

			if (frame->FrameState == FRAME_OK) // || (frame->FrameState == FRAME_OVERFLOW))
			// if (true)
			{

				receivedStats.okFrames++;
				receivedStats.bytes += frame->Length(); // includes CRC bytes


				if (systemState == DEBUG)
				{

					// Write a user readable frame to serial port
					if (echoToSerial)
					{
						printf("\r\n");

						if (echoToSerial)
						{
							printf("-------------------------------------------------\r\n");
							frame->DumpFrame();
							printf("-------------------------------------------------\r\n");
						}
					}

					processFrame(frame);
				}
				else
				{
					SendByteStuffed(frame);
				}
			}
			else
			{
				receivedStats.badFrames++;
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