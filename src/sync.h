
#ifndef SYNC_H
#define SYNC_H

#include "hdlc.h"

void processPioData(uint8_t data);
void initialize_sync_tasks();
void sendHdlcByte(uint8_t data, bool isFlag);
void inspectCurrentHdlcFrame();
void sendHdlcByte(uint8_t data, bool isFlag);
void SendHDLCFrame(HDLC *frame);
void SendByteStuffed(HDLC *frame);
// Worst-case size of one byte-stuffed frame: every byte escaped, plus two flags
#define BYTE_STUFFED_MAX (2 * HDLC::HDLC_MAX_FRAME_LENGTH + 2)
int AppendByteStuffed(HDLC *frame, uint8_t *dst); // encode one frame into dst, returns its length
bool applyTxBaud(uint32_t bps); // reprogram the PIO transmitter's clock divider at runtime
extern volatile bool txPioReady;  // true once the PIO transmitter is initialised
void processSerialByteStuffed(uint8_t data);
void resetByteStuffedReceiver();

// Receive path measurements (line -> Pico -> USB) for STAT
struct RxPathStats
{
	uint32_t lineBytes;      // bytes the PIO receiver delivered
	uint32_t lineIdleBytes;  // of those, all-ones idle fill bytes
	uint32_t queueDepthMax;  // deepest the received-frame queue got
	uint32_t queueSize;      // its capacity
	uint32_t latencyMaxUs;   // frame closed by the interrupt -> USB write returned, longest
	uint64_t latencyTotalUs; // the same, summed
	uint32_t latencyCount;   // frames in the sum
	uint64_t waitTotalUs;    // RX task time blocked waiting for a frame
	uint64_t windowUs;       // time since the counters were last cleared (or boot)
	uint32_t dmaOverruns;    // times the DMA receive ring was lapped (bytes lost; must stay 0)
	uint32_t dmaMaxBacklog;  // most bytes waiting in the DMA receive ring when the decoder looked
	uint32_t dmaRingSize;    // the ring's size in bytes
	bool pioRxStall;         // PIO FDEBUG RXSTALL for the receive state machine: the 8-entry receive
	                         // FIFO was full and a byte was thrown away (must stay false)
};
void rxPathStatsGet(RxPathStats *out);
void rxPathStatsClear(); // "STAT CLEAR"; the RX task clears its share when the next frame arrives

#endif