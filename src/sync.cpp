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

// 32 frames: at 307200 bit/s a burst of 10-byte frames arrives every 0.5 ms, and with 10 the queue
// ran dry while one USB write was in progress (2026-11-09 full test: 4110 of 15366 frames dropped).
#define MAX_FRAME_QUEUE_SIZE 32

// DMA receive ring size (see rx_dma_start)
#define RX_DMA_RING_BITS 13
#define RX_DMA_RING_SIZE (1u << RX_DMA_RING_BITS)
QueueHandle_t hdlcQueue = nullptr;

// Frame objects are NOT heap allocated. The PIO RX interrupt used to do "new HDLC()" for every
// completed frame and the RX task did "delete". On the Pico SDK, new/delete go through the SDK's
// malloc wrapper, which takes a mutex and then calls the libc heap - neither is allowed from an
// interrupt, and the task's delete races with the interrupt's new. Instead there is a fixed pool:
// one object is being filled by the interrupt, up to MAX_FRAME_QUEUE_SIZE are waiting in hdlcQueue
// for the RX task, and the rest sit in freeFrameQueue. Since 2026-11-09 the frames are decoded by the
// decode task (from the DMA receive ring), not in an interrupt, so both sides use the task API.
#define FRAME_POOL_SIZE (MAX_FRAME_QUEUE_SIZE + 2)
static HDLC framePool[FRAME_POOL_SIZE];
QueueHandle_t freeFrameQueue = nullptr; // HDLC* objects the RX task has finished with, ready for reuse

HDLC *rxHDLC = nullptr; // The frame object the PIO RX interrupt is currently filling

// Copy of the most recent BAD received frame (filled by the RX task), readable over the debug probe.
uint8_t lastBadBytes[HDLC::HDLC_MAX_FRAME_LENGTH];
int lastBadLen = 0;   // bytes in lastBadBytes
int lastBadState = 0; // FRAME_STATE value of that frame (FRAME_ERROR, FRAME_OVERFLOW, FRAME_ABORT)
int lastBadSeq = 0;   // receivedStats.totalFrames at the time, i.e. which frame it was



// ---- Receive path measurements (line -> Pico -> USB), shown by STAT, cleared by "STAT CLEAR" ----
// Written by the decode task:
volatile uint32_t rxLineBytes = 0;     // every byte the PIO receiver delivered while hdlcMode is on
volatile uint32_t rxLineIdleBytes = 0; // of those, bytes that are all ones (0xFF). Eight ones in a row
                                       // cannot occur inside a frame or a flag (stuffing stops at five),
                                       // so these are certainly idle fill. Bytes that straddle a frame
                                       // edge (partly fill, partly flag) count as non-idle.
volatile uint32_t rxQueueDepthMax = 0; // deepest hdlcQueue seen right after a frame was queued
static uint32_t frameCloseUs[FRAME_POOL_SIZE]; // time_us_32() when the decode task queued each pool object
volatile uint32_t rxDmaOverruns = 0;   // times the DMA writer lapped the decoder (bytes lost; must stay 0)
volatile uint32_t rxDmaMaxBacklog = 0; // most bytes waiting in the DMA ring when the decoder looked
volatile uint64_t rxDecodeBusyUs = 0;  // CPU time the decode task spent decoding (it runs above every
                                       // other task, so this time is taken from all of them)
volatile uint32_t rxDecodeSlowBytes = 0; // bytes decoded bit by bit (not the all-ones fast path)
// Written by the RX task:
static uint32_t rxLatencyMaxUs = 0;    // frame queued by the interrupt -> its USB write returned, longest
static uint64_t rxLatencyTotalUs = 0;  // the same, summed over all forwarded frames
static uint32_t rxLatencyCount = 0;    // frames that went into the two numbers above
static uint64_t rxWaitTotalUs = 0;     // time the RX task spent blocked waiting on hdlcQueue
static volatile uint64_t rxMeasureStartUs = 0; // when STAT CLEAR last ran (boot = 0). Set by the
                                       // clear itself, so the window is right even in a run where
                                       // no frame arrives (it used to restart on the next frame)

void rxPathStatsClear()
{
	rxLineBytes = 0;
	rxLineIdleBytes = 0;
	rxQueueDepthMax = 0;
	rxDmaOverruns = 0;
	rxDmaMaxBacklog = 0;
	rxMeasureStartUs = time_us_64();
	rxDecodeBusyUs = 0;
	rxDecodeSlowBytes = 0;
	pio_rx->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + sm_rx); // write 1 to clear the sticky flag
	// The RX task's own totals are cleared here too (they used to wait for the next received frame,
	// so a run with no frames kept the old numbers). STAT CLEAR is sent between runs, while the
	// RX task is idle, so there is no update to collide with.
	rxLatencyMaxUs = 0;
	rxLatencyTotalUs = 0;
	rxLatencyCount = 0;
	rxWaitTotalUs = 0;
}

void rxPathStatsGet(RxPathStats *out)
{
	out->lineBytes = rxLineBytes;
	out->lineIdleBytes = rxLineIdleBytes;
	out->queueDepthMax = rxQueueDepthMax;
	out->queueSize = FRAME_POOL_SIZE;
	out->latencyMaxUs = rxLatencyMaxUs;
	out->latencyTotalUs = rxLatencyTotalUs;
	out->latencyCount = rxLatencyCount;
	out->waitTotalUs = rxWaitTotalUs;
	out->windowUs = time_us_64() - rxMeasureStartUs;
	out->dmaOverruns = rxDmaOverruns;
	out->dmaMaxBacklog = rxDmaMaxBacklog;
	out->dmaRingSize = RX_DMA_RING_SIZE;
	out->decodeBusyUs = rxDecodeBusyUs;
	out->decodeSlowBytes = rxDecodeSlowBytes;
	out->pioRxStall = (pio_rx->fdebug & (1u << (PIO_FDEBUG_RXSTALL_LSB + sm_rx))) != 0;
}

// Called by the decode task when rxHDLC holds a complete frame.
// Hands the frame to the RX task through hdlcQueue and takes a fresh object from the free pool.
// If the pool is empty (the RX task is behind), the frame is dropped and counted; the same object
// is cleared and reused. Never blocks: the decode task must keep up with the line.
void __not_in_flash_func(queueHDLCFrame)()
{
	HDLC *fresh = nullptr;

	if (xQueueReceive(freeFrameQueue, &fresh, 0) != pdPASS)
	{
		// No free object: drop this frame, keep filling the same object
		receivedStats.droppedFrames++;
		rxHDLC->Clear();
		return;
	}

	frameCloseUs[rxHDLC - framePool] = time_us_32(); // start of this frame's receive -> USB latency

	if (xQueueSend(hdlcQueue, &rxHDLC, 0) != pdPASS)
	{
		// Cannot happen while both queues hold FRAME_POOL_SIZE entries, but never leak an object
		receivedStats.droppedFrames++;
		rxHDLC->Clear();
		xQueueSend(freeFrameQueue, &fresh, 0);
		return;
	}

	{
		uint32_t depth = (uint32_t)uxQueueMessagesWaiting(hdlcQueue);
		if (depth > rxQueueDepthMax)
		{
			rxQueueDepthMax = depth;
		}
	}

	rxHDLC = fresh;
	// The flag that just closed the previous frame also opens this one
	rxHDLC->BeginFrameAfterFlag();
}

// Called by the RX task when it is done with a frame taken from hdlcQueue.
void __not_in_flash_func(releaseHDLCFrame)(HDLC *frame)
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
void __not_in_flash_func(processPioData)(uint8_t data)
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
		// All-ones byte while hunting for a flag: cannot contain a flag, skip the 8 single-bit steps.
		// At 983000 bit/s about 9 of 10 line bytes are idle fill, and the per-bit loop on them was
		// the bulk of the receive CPU time.
		if (rxHDLC->TryIdleOnesByte(data))
		{
			return;
		}
		rxDecodeSlowBytes++;

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

// One byte from the PIO receiver, in line order. Called only by the decode task.
static void __not_in_flash_func(rxConsumeByte)(uint8_t data)
{
	if (!rawRingFrozen)
	{
		rawRing[rawRingPos % RAW_RING_SIZE] = data;
		rawRingPos++;
	}

	if (hdlcMode)
	{
		rxLineBytes++;
		if (data == 0xFF)
		{
			rxLineIdleBytes++;
		}
		processPioData(data);
	}
	else
	{
		receiveBuffer->writeBuffer(data);
	}
}

// ---- DMA receive ring ----
// Until 2026-11-09 a PIO interrupt fired for every received byte and decoded it bit by bit inside
// the interrupt. At 983000 bit/s a byte arrives every 8.1 us; the interrupt could not keep up, the
// PIO receive FIFO overflowed (FDEBUG RXSTALL set) and 2000..7300 line bytes per run were lost
// (full test 2026-11-09: 1890 bad, 58 aborted frames). Now a DMA channel copies every byte from the
// FIFO into this ring with no CPU involvement, and the decode task works through the ring once per
// millisecond. 8 KB holds 66 ms of line at 983000 bit/s.
//
// The channel writes with ring wrap (the buffer is aligned to its own size) and a huge transfer
// count, so the count register tells exactly how many bytes have arrived: no race between a wrap
// counter and an address. A second channel re-arms the count when it runs out (after 2^32-1 bytes,
// about 9.7 hours at 983000 bit/s); the write address carries on from where it was.
// RX_DMA_RING_BITS / RX_DMA_RING_SIZE are defined at the top of the file
static uint8_t rxDmaRing[RX_DMA_RING_SIZE] __attribute__((aligned(RX_DMA_RING_SIZE)));
static int rxDmaChan = -1;     // copies PIO RX FIFO -> rxDmaRing
static int rxDmaCtrlChan = -1; // re-arms rxDmaChan's transfer count when it reaches 0
static const uint32_t rxDmaFullCount = 0xFFFFFFFFu;

static void rx_dma_start(PIO pio, uint sm)
{
	rxDmaChan = dma_claim_unused_channel(true);
	rxDmaCtrlChan = dma_claim_unused_channel(true);

	// Data channel: one byte per FIFO entry. The receive program shifts right, so the 8 received
	// bits sit in the top byte of the 32-bit FIFO word; a byte read at offset 3 returns that lane
	// (same as sync_rx_program_getc in sdlc.pio).
	dma_channel_config c = dma_channel_get_default_config(rxDmaChan);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_ring(&c, true, RX_DMA_RING_BITS); // wrap the write address inside the ring
	channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));
	channel_config_set_chain_to(&c, rxDmaCtrlChan);
	dma_channel_configure(rxDmaChan, &c, rxDmaRing, (io_rw_8 *)&pio->rxf[sm] + 3, rxDmaFullCount, false);

	// Control channel: writes the full count into the data channel's count-and-trigger register.
	dma_channel_config cc = dma_channel_get_default_config(rxDmaCtrlChan);
	channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
	channel_config_set_read_increment(&cc, false);
	channel_config_set_write_increment(&cc, false);
	dma_channel_configure(rxDmaCtrlChan, &cc, &dma_hw->ch[rxDmaChan].al1_transfer_count_trig,
						  &rxDmaFullCount, 1, false);

	dma_channel_start(rxDmaChan);
}

// Total bytes the DMA has written since it started, as a running 64-bit count. Called only by the
// decode task, which looks far more often than once per re-arm period (hours).
static uint64_t rxDmaTotal = 0;
static uint32_t rxDmaLastRemaining = 0xFFFFFFFFu;
static uint64_t __not_in_flash_func(rx_dma_written)()
{
	uint32_t remaining = dma_hw->ch[rxDmaChan].transfer_count;
	if (remaining <= rxDmaLastRemaining)
	{
		rxDmaTotal += rxDmaLastRemaining - remaining;
	}
	else
	{
		// re-armed since the last look: the old run finished at 0, the new one started at full
		rxDmaTotal += rxDmaLastRemaining + (rxDmaFullCount - remaining);
	}
	rxDmaLastRemaining = remaining;
	return rxDmaTotal;
}

// Highest priority task: decodes everything the DMA has written, then sleeps one tick.
#define RX_DECODE_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
static void __not_in_flash_func(tskRxDecode)(void *pvParameters)
{
	(void)pvParameters;
	uint64_t readTotal = 0;

	while (true)
	{
		uint64_t written = rx_dma_written();
		uint64_t backlog = written - readTotal;
		if (backlog > rxDmaMaxBacklog)
		{
			rxDmaMaxBacklog = (uint32_t)((backlog > 0xFFFFFFFFu) ? 0xFFFFFFFFu : backlog);
		}
		if (backlog > RX_DMA_RING_SIZE)
		{
			// The DMA lapped us: the oldest bytes are already overwritten. Count it, drop the frame in
			// progress (its bits are gone) and carry on with the oldest bytes still in the ring.
			rxDmaOverruns++;
			readTotal = written - RX_DMA_RING_SIZE;
			if (rxHDLC != nullptr)
			{
				rxHDLC->Clear();
			}
		}

		uint32_t busyStart = time_us_32();
		bool didWork = (readTotal < written);
		while (readTotal < written)
		{
			rxConsumeByte(rxDmaRing[(uint32_t)readTotal & (RX_DMA_RING_SIZE - 1)]);
			readTotal++;
		}

		if (didWork)
		{
			rxDecodeBusyUs += time_us_32() - busyStart;
		}

		vTaskDelay(1);
	}
}

// Task waiting for room in the PIO transmit FIFO, woken by txFifoIrqHandler (notification index 1;
// index 0 is the console task's USB wake-up)
static TaskHandle_t txFifoWaiter = NULL;
// Transmit-side measurements for STAT, cleared by STAT CLEAR
static volatile uint32_t txFifoWaits = 0;  // times a sender found the PIO TX FIFO full and slept
static volatile uint64_t txFifoWaitUs = 0; // total time spent in those sleeps

void txPathStatsGet(uint32_t *waits, uint64_t *waitUs)
{
	*waits = txFifoWaits;
	*waitUs = txFifoWaitUs;
}

void txPathStatsClear()
{
	txFifoWaits = 0;
	txFifoWaitUs = 0;
}
static uint txFifoIrqSm = 0;
#define TX_FIFO_NOTIFY_INDEX 1

// PIO0_IRQ_1: the transmit FIFO has room. The source is level-triggered (it stays asserted
// while the FIFO is not full), so the handler switches it off before waking the waiting task.
static void __not_in_flash_func(txFifoIrqHandler)(void)
{
	pio_set_irq1_source_enabled(pio_tx, (pio_interrupt_source_t)(PIO_INTR_SM0_TXNFULL_LSB + txFifoIrqSm), false);
	if (txFifoWaiter != NULL)
	{
		BaseType_t woken = pdFALSE;
		vTaskNotifyGiveIndexedFromISR(txFifoWaiter, TX_FIFO_NOTIFY_INDEX, &woken);
		portYIELD_FROM_ISR(woken);
	}
}

static void setup_tx_fifo_interrupt(PIO pio, uint sm)
{
	txFifoIrqSm = sm;
	uint irqNum = (pio == pio0) ? PIO0_IRQ_1 : PIO1_IRQ_1;
	pio_set_irq1_source_enabled(pio, (pio_interrupt_source_t)(PIO_INTR_SM0_TXNFULL_LSB + sm), false);
	irq_set_exclusive_handler(irqNum, txFifoIrqHandler);
	irq_set_enabled(irqNum, true);
}

void __not_in_flash_func(send_byte)(PIO pio, uint sm, uint8_t byte)
{
	// Wait until there is space in the FIFO. This used to sleep one 1 ms tick per check; the
	// 8-entry FIFO empties in 1.4 ms at 57600 and 0.7 ms at 115200 bit/s, so the line clock
	// stopped between refills. Now the FIFO-has-room interrupt wakes the task at once. The 2 ms
	// timeout only guards against a lost wake-up; the loop always re-checks the FIFO itself.
	while (pio_sm_is_tx_fifo_full(pio, sm))
	{
		txFifoWaits++; // one wake-up per pass: at high rates this can be one per byte
		uint32_t waitStart = time_us_32();
		txFifoWaiter = xTaskGetCurrentTaskHandle();
		ulTaskNotifyValueClearIndexed(NULL, TX_FIFO_NOTIFY_INDEX, 0xFFFFFFFFu);
		pio_set_irq1_source_enabled(pio, (pio_interrupt_source_t)(PIO_INTR_SM0_TXNFULL_LSB + sm), true);
		if (pio_sm_is_tx_fifo_full(pio, sm))
		{
			ulTaskNotifyTakeIndexed(TX_FIFO_NOTIFY_INDEX, pdTRUE, pdMS_TO_TICKS(2));
		}
		pio_set_irq1_source_enabled(pio, (pio_interrupt_source_t)(PIO_INTR_SM0_TXNFULL_LSB + sm), false);
		txFifoWaitUs += time_us_32() - waitStart;
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

void __not_in_flash_func(sendHdlcByte)(uint8_t data, bool isFlag)
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

// Record of every frame forwarded to the USB host (read over the debug probe)
#define FWD_LOG_SIZE 1024
struct FwdLogEntry
{
	uint32_t ms;
	uint16_t stuffedLen;
	uint8_t head[8];
};
FwdLogEntry fwdLog[FWD_LOG_SIZE];
volatile uint32_t fwdLogPos = 0;

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
	static uint8_t out[BYTE_STUFFED_MAX];

	int n = AppendByteStuffed(frame, out);
	console_out_usb_write(out, n);
}

// Encodes one frame (RFC 1662 byte stuffing, both flags) into dst, which must have room for
// BYTE_STUFFED_MAX bytes. Returns the number of bytes written. Also records the frame in fwdLog.
// Used by SendByteStuffed (one frame, one USB write) and by the RX task, which packs every frame
// waiting in its queue into ONE USB write: one write per frame took about 1 ms (one USB frame
// interval), which capped forwarding near 1000 frames/s and lost 4110 of 15366 10-byte frames at
// 307200 bit/s in the 2026-11-09 full test.
int __not_in_flash_func(AppendByteStuffed)(HDLC *frame, uint8_t *dst)
{
	uint8_t raw[HDLC::HDLC_MAX_FRAME_LENGTH];

	int len = frame->Length();
	if (len > (int)sizeof(raw))
	{
		len = sizeof(raw);
	}
	for (int i = 0; i < len; i++)
	{
		raw[i] = frame->GetFrameByte(i);
	}

	int n = byte_frame_encode(raw, len, dst);

	// Per-frame record for matching lost frames against writes over the debug probe:
	// first 8 frame bytes (address, control, the test tool's sequence number), stuffed length, ms
	fwdLog[fwdLogPos % FWD_LOG_SIZE].ms = to_ms_since_boot(get_absolute_time());
	fwdLog[fwdLogPos % FWD_LOG_SIZE].stuffedLen = (uint16_t)n;
	for (int i = 0; i < 8; i++)
	{
		fwdLog[fwdLogPos % FWD_LOG_SIZE].head[i] = (i < len) ? raw[i] : 0;
	}
	fwdLogPos++;

	return n;
}

// Byte-stuffed frame reception state machine
enum ByteStuffedState
{
	BS_IDLE,				// Waiting for frame start
	BS_RECEIVING_DATA,		// Receiving frame data
	BS_ESCAPE_RECEIVED		// Previous byte was escape character
};

ByteStuffedState byteStuffedState = BS_IDLE;
uint8_t byteStuffedBuffer[HDLC::HDLC_MAX_FRAME_LENGTH]; // incoming frame from the USB host, un-stuffed
volatile uint32_t usbRxOversizeFrames = 0; // frames from the USB host longer than the buffer: thrown away
int byteStuffedPos = 0;          // Current position in buffer
int escapeCount = 0;             // Count of consecutive ESC characters
int receivedFrameCount = 0;      // Count of received frames in current session

void resetByteStuffedReceiver()
{
	byteStuffedState = BS_IDLE;
	byteStuffedPos = 0;
	escapeCount = 0;
}

void __not_in_flash_func(processSerialByteStuffed)(uint8_t data)
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
					// Frame longer than HDLC_MAX_FRAME_LENGTH: count it and throw it away. No printf:
					// the text would go to the host in the middle of its frame stream.
					usbRxOversizeFrames++;
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
				usbRxOversizeFrames++;
				resetByteStuffedReceiver();
			}
			break;

	}
}

// Apply a new transmit line rate to the running PIO transmitter. Until 2026-11-08 BAUD only
// stored the number; the PIO clock divider was computed once when the TX task started, so a
// BAUD command changed nothing on the line. The divider is sys_clk / (bps * 33) because one bit
// is 33 PIO cycles (sync_tx_with_count in sdlc.pio). The change is made only when the TX FIFO is
// empty, so a frame in flight keeps its rate; waits at most 500 ms for that.
volatile bool txPioReady = false; // set once the TX task has initialised the PIO transmitter

bool applyTxBaud(uint32_t bps)
{
	if (!txPioReady || bps == 0)
	{
		return false; // transmitter not started yet; the TX task applies currentBaudRate itself
	}

	int waited = 0;
	while (!pio_sm_is_tx_fifo_empty(pio_tx, sm_tx))
	{
		vTaskDelay(pdMS_TO_TICKS(1));
		if (++waited > 500)
		{
			return false; // the line never went quiet; keep the old rate
		}
	}
	// One PIO word is still being shifted out when the FIFO goes empty: a byte takes at most
	// 10 bits * 33 cycles, well under 20 ms even at 9600, so a short wait lets it finish.
	vTaskDelay(pdMS_TO_TICKS(20));

	float div = (float)clock_get_hz(clk_sys) / ((float)bps * 33.0f);
	pio_sm_set_clkdiv(pio_tx, sm_tx, div);
	pio_sm_clkdiv_restart(pio_tx, sm_tx);
	return true;
}

// Inter-frame time fill sent after every closing flag, before the transmit clock stops.
// Decided by Ronny 2026-11-08 after measuring the ESP32 receiver: its SPI DMA only hands a
// 4-byte word to software once at least one more byte has arrived after it, so a frame whose
// closing flag sits in the last word is invisible until the next transmission. Four bytes of
// ones (line held at mark, legal HDLC idle) push every frame all the way through. Must be >= 4.
#define HDLC_TX_FILL_BYTES 4

void __not_in_flash_func(SendHDLCFrame)(HDLC *frame)
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
	setup_tx_fifo_interrupt(pio_tx, sm_tx);
	txPioReady = true; // from here on BAUD reprograms the divider through applyTxBaud()

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

	printf("Initializing RX DMA ring and decode task\r\n");
	rx_dma_start(pio_rx, sm_rx);
	xTaskCreate(tskRxDecode, "RxDecode", 1 * 1024, NULL, RX_DECODE_TASK_PRIORITY, NULL);

	// Waiting for HDLC frames to process
	while (true)
	{
		HDLC *frame;

		// Process the HDLC frame here
		uint64_t waitStart = time_us_64();
		while (xQueueReceive(hdlcQueue, &frame, portMAX_DELAY) == pdPASS)
		{
			// Count only waiting inside the current window: a wait that began before STAT CLEAR
			// counts from the clear onwards.
			if (waitStart < rxMeasureStartUs)
			{
				waitStart = rxMeasureStartUs;
			}
			rxWaitTotalUs += time_us_64() - waitStart;

			// Take this frame and every other frame already waiting, and forward all of them to the
			// USB host in ONE write (see AppendByteStuffed for why). The batch ends when the queue
			// is empty or the buffer could not take another worst-case frame.
			static uint8_t usbBatch[2 * BYTE_STUFFED_MAX]; // always room for at least one worst-case frame
			// When each frame in usbBatch closed on the line, for the latency measurement. The frame
			// object itself goes back to the pool as soon as its bytes are copied into usbBatch: it used
			// to be held until the USB write finished (up to 4.9 ms), so a burst of small frames at
			// 983000 bit/s could use up all FRAME_POOL_SIZE slots during one write and be DROPPED.
			uint32_t pendingCloseUs[FRAME_POOL_SIZE];
			int pendingCount = 0;
			int batchLen = 0;
			bool haveFrame = true;
			while (haveFrame)
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
					batchLen += AppendByteStuffed(frame, usbBatch + batchLen);
					pendingCloseUs[pendingCount++] = frameCloseUs[frame - framePool];
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

			// Give the frame object back to the pool now: its bytes, if forwarded, are in usbBatch
			releaseHDLCFrame(frame);

			// Next frame for this batch, if one is already waiting and there is room for it
			haveFrame = ((batchLen + BYTE_STUFFED_MAX) <= (int)sizeof(usbBatch)) &&
						(pendingCount < FRAME_POOL_SIZE) &&
						(xQueueReceive(hdlcQueue, &frame, 0) == pdPASS);
			} // while (haveFrame)

			if (batchLen > 0)
			{
				console_out_usb_write(usbBatch, batchLen);

				uint32_t now = time_us_32();
				for (int k = 0; k < pendingCount; k++)
				{
					uint32_t latency = now - pendingCloseUs[k];
					if (latency > rxLatencyMaxUs)
					{
						rxLatencyMaxUs = latency;
					}
					rxLatencyTotalUs += latency;
					rxLatencyCount++;
				}
			}
			waitStart = time_us_64(); // from here the task is waiting for the next frame
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
				2 * 1024,		  // words; AppendByteStuffed keeps a 1024-byte frame copy on this stack
				//		  // The size of the stack to allocate to the task.
				NULL,			  // The parameter passed to the task - not used in this case.
				RX_TASK_PRIORITY, // The priority assigned to the task.
				&handleRX);

	// Pin Tasks
#if (configNUMBER_OF_CORES > 1)
	vTaskCoreAffinitySet(handleTX, (1 << 1)); // Core 0
	vTaskCoreAffinitySet(handleRX, (1 << 0)); // Core 1
#endif
}