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
#include "control_frame.h"
#include "console_out.h"

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

// Frame objects are NOT heap allocated. The PIO RX interrupt used to do "new HDLC()" for every
// completed frame and the RX task did "delete". On the Pico SDK, new/delete go through the SDK's
// malloc wrapper, which takes a mutex and then calls the libc heap - neither is allowed from an
// interrupt, and the task's delete races with the interrupt's new. Instead there is a fixed pool:
// one object is being filled by the interrupt, up to MAX_FRAME_QUEUE_SIZE are waiting in hdlcQueue
// for the RX task, and the rest sit in freeFrameQueue. Both queues are only touched with the
// FromISR / task variants of the FreeRTOS queue API, which are interrupt safe.
#define FRAME_POOL_SIZE (MAX_FRAME_QUEUE_SIZE + 2)
static HDLC framePool[FRAME_POOL_SIZE];
QueueHandle_t freeFrameQueue = nullptr; // HDLC* objects the RX task has finished with, ready for reuse

HDLC *rxHDLC = nullptr; // The frame object the PIO RX interrupt is currently filling

// Copy of the most recent BAD received frame (filled by the RX task), readable over the debug probe.
uint8_t lastBadBytes[HDLC::HDLC_MAX_FRAME_LENGTH];
int lastBadLen = 0;   // bytes in lastBadBytes
int lastBadState = 0; // FRAME_STATE value of that frame (FRAME_ERROR, FRAME_OVERFLOW, FRAME_ABORT)
int lastBadSeq = 0;   // receivedStats.totalFrames at the time, i.e. which frame it was



// Called from the PIO RX interrupt when rxHDLC holds a complete frame.
// Hands the frame to the RX task through hdlcQueue and takes a fresh object from the free pool.
// If the pool is empty (the RX task is behind), the frame is dropped and counted; the same object
// is cleared and reused. No heap, no printf: this runs in interrupt context.
void queueHDLCFrame()
{
	HDLC *fresh = nullptr;

	if (xQueueReceiveFromISR(freeFrameQueue, &fresh, NULL) != pdPASS)
	{
		// No free object: drop this frame, keep filling the same object
		receivedStats.droppedFrames++;
		rxHDLC->Clear();
		return;
	}

	if (xQueueSendFromISR(hdlcQueue, &rxHDLC, NULL) != pdPASS)
	{
		// Cannot happen while both queues hold FRAME_POOL_SIZE entries, but never leak an object
		receivedStats.droppedFrames++;
		rxHDLC->Clear();
		xQueueSendFromISR(freeFrameQueue, &fresh, NULL);
		return;
	}

	rxHDLC = fresh;
	// The flag that just closed the previous frame also opens this one
	rxHDLC->BeginFrameAfterFlag();
}

// Called by the RX task when it is done with a frame taken from hdlcQueue.
void releaseHDLCFrame(HDLC *frame)
{
	frame->Clear();
	xQueueSend(freeFrameQueue, &frame, 0);
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

// Raw record of every byte the PIO receiver delivered, in order, for offline analysis over the
// debug probe (RULE: decode the actual bytes). 16 KB holds about two 60-frame bursts at 126 bytes.
// Written only by the interrupt; rawRingPos keeps counting past the end so the reader knows how
// many bytes arrived in total and where the newest one is (pos % size).
#define RAW_RING_SIZE 16384
uint8_t rawRing[RAW_RING_SIZE];
volatile uint32_t rawRingPos = 0;
// The RX task sets rawRingFrozen when it sees the first bad frame, so the ring keeps the bytes
// leading up to and including that frame instead of being overwritten by later traffic (the
// ESP32's idle-ones clock alone fills the ring every 7 s at 19200 bit/s).
volatile bool rawRingFrozen = false;
volatile uint32_t rawRingFreezePos = 0;

void pio_irq_handler()
{
	// Process the received data
	// Write data from PIO to receiveBuffer or HDLC frame
	while (!pio_sm_is_rx_fifo_empty(pio_rx, sm_rx))
	{
		uint8_t data = sync_rx_program_getc(pio_rx, sm_rx);

		if (!rawRingFrozen)
		{
			rawRing[rawRingPos % RAW_RING_SIZE] = data;
			rawRingPos++;
		}

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
		// Flags (and inter-frame fill) go out as they are, no stuffing. A flag also ends any run
		// of ones, so the stuffing counter must restart at zero for the bytes that follow it;
		// otherwise ones left over from the previous frame's last byte would cause a stuffed zero
		// too early in the next frame.
		tx_data = data;
		sendbits = 8;
		ones = 0;
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

	// Build the whole byte-stuffed frame first and hand it to the USB driver in ONE write.
	// Why: the USB stdio driver takes a mutex per call and only has a 64-byte CDC buffer; written
	// byte by byte (as before) a 126-byte frame meant ~260 mutex round trips against the console
	// task's input polling, and the driver drops bytes without any feedback when it cannot get the
	// mutex or buffer space in time. In the 2026-11-08 ESP32 traffic run two frames reached the host
	// truncated for that reason. One write per frame removes the per-byte contention; the driver's
	// own wait-for-space loop then handles the 64-byte chunks. The write goes straight to the USB
	// driver (console_out_usb_write), not through printf, so it is unaffected when console_out has
	// switched printf over to a REPLY-frame capture. Worst case: every byte escaped, plus two flags.
	static uint8_t raw[HDLC::HDLC_MAX_FRAME_LENGTH];
	static uint8_t out[2 * HDLC::HDLC_MAX_FRAME_LENGTH + 2];

	int len = frame->Length();
	if (len > (int)sizeof(raw))
	{
		len = sizeof(raw);
	}
	for (int i = 0; i < len; i++)
	{
		raw[i] = frame->GetFrameByte(i);
	}

	int n = byte_frame_encode(raw, len, out);
	console_out_usb_write(out, n);
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
				console_out_event(CONTROL_FRAME_EVENT_MODE_CHANGE, "DEBUG"); // sent as a frame while still in FORWARDING
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
					control_frame cf;
					if (control_frame_parse(byteStuffedBuffer, byteStuffedPos, &cf))
					{
						// A control frame (address 0xFE, un-inverted CRC): for this firmware,
						// never for the line, and it does not count as a forwarded frame.
						console_out_handle_control_frame(&cf);
					}
					else
					{
					// Create HDLC frame from received data and send to HDLC interface
					HDLC newFrame;
					newFrame.CreateFrameFromByteStuffed(byteStuffedBuffer, byteStuffedPos);
					SendHDLCFrame(&newFrame);

					receivedFrameCount++;
					}
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

// Inter-frame time fill sent after every closing flag, before the transmit clock stops.
// Decided by Ronny 2026-11-08 after measuring the ESP32 receiver: its SPI DMA only hands a
// 4-byte word to software once at least one more byte has arrived after it, so a frame whose
// closing flag sits in the last word is invisible until the next transmission. Four bytes of
// ones (line held at mark, legal HDLC idle) push every frame all the way through. Must be >= 4.
#define HDLC_TX_FILL_BYTES 4

void SendHDLCFrame(HDLC *frame)
{
	// for (int x = 0; x < 1000; x++)
	{
		sendHdlcByte(HDLC::HDLC_FLAG, true);
		int len = frame->Length();
		for (int i = 0; i < len; i++)
		{
			uint8_t data = frame->GetFrameByte(i);

			// Every byte inside the frame is DATA and must be bit-stuffed, including a data byte
			// that happens to be 0x7E. Bit stuffing is what makes it different from a flag on the
			// wire (0x7E = 01111110 stuffed becomes 0111110 10, no six ones in a row). The old code
			// sent a 0x7E data byte raw as a flag, which split the frame in two for the receiver.
			// Found 2026-11-08: 126-byte random payloads to the ESP32 all failed FCS.
			sendHdlcByte(data, false);
		}

		sentStats.okFrames++;
		sentStats.totalFrames++;
		sentStats.bytes += len;
	}
	// Closing flag, then the inter-frame fill of ones (sent unstuffed like a flag), then the
	// transmit clock stops by itself when the PIO FIFO runs empty.
	sendHdlcByte(HDLC::HDLC_FLAG, true);
	for (int i = 0; i < HDLC_TX_FILL_BYTES; i++)
	{
		sendHdlcByte(0xFF, true);
	}
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

	// Frame pool: object 0 is the one the interrupt fills, the others start in the free queue.
	// Both queues are sized to hold the whole pool so a send can never fail for lack of space.
	hdlcQueue = xQueueCreate(FRAME_POOL_SIZE, sizeof(HDLC *));
	freeFrameQueue = xQueueCreate(FRAME_POOL_SIZE, sizeof(HDLC *));
	if ((hdlcQueue == nullptr) || (freeFrameQueue == nullptr))
	{
		panic("Unable to create HDLC frame queues");
	}

	rxHDLC = &framePool[0];
	for (int i = 1; i < FRAME_POOL_SIZE; i++)
	{
		HDLC *p = &framePool[i];
		xQueueSend(freeFrameQueue, &p, 0);
	}

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

				// Keep a copy of the most recent bad frame in plain globals, so it can be read out
				// over the debug probe (or by INSPECT) even in FORWARDING mode where nothing is printed.
				lastBadLen = frame->Length();
				if (lastBadLen > (int)sizeof(lastBadBytes)) lastBadLen = sizeof(lastBadBytes);
				for (int i = 0; i < lastBadLen; i++)
				{
					lastBadBytes[i] = frame->GetFrameByte(i);
				}
				lastBadState = (int)frame->FrameState;
				lastBadSeq = receivedStats.totalFrames; // which received frame (1-based) it was

				// First bad frame: stop the raw ring so the bytes around it can be read out
				if (!rawRingFrozen)
				{
					rawRingFreezePos = rawRingPos;
					rawRingFrozen = true;
				}

				if (frame->FrameState == FRAME_OVERFLOW)
				{
					receivedStats.overflowFrames++;
				}
				else if (frame->FrameState == FRAME_ABORT)
				{
					receivedStats.abortedFrames++;
				}

				// In DEBUG mode with echo on, show the bad frame too, so a wrong CRC, an abort or a
				// merged (overflowed) frame can be seen byte by byte instead of only counted.
				if ((systemState == DEBUG) && echoToSerial)
				{
					const char *why = "CRC error or odd bit count";
					if (frame->FrameState == FRAME_OVERFLOW) why = "overflow";
					if (frame->FrameState == FRAME_ABORT) why = "abort";
					printf("\r\n----- BAD frame (%s) -----\r\n", why);
					frame->DumpFrame();
					printf("-------------------------------------------------\r\n");
				}
			}

			// Give the frame object back to the pool for the interrupt to reuse
			releaseHDLCFrame(frame);
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