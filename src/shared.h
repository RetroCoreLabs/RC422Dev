#ifndef SHARED_H
#define SHARED_H

#include <stdio.h>
#include "RingBuffer.h"
#include "hdlc.h"

bool InitializeSharedObjects();

extern RingBuffer* receiveBuffer;
extern RingBuffer* sendBuffer;

extern bool echoToSerial;

extern bool loopbackEnabled;
extern bool echoToSerial;
extern bool hdlcMode;

extern uint32_t defaultBaudRate;
extern uint32_t currentBaudRate;
extern uint32_t maxBaudRate;

// Loopback flag
extern bool loopbackEnabled;

// Send received data to USB Serial port
extern bool echoToSerial;

// HDLC mode ?
extern bool hdlcMode;

// Firmware version reported to the host in the HELLO reply (control_frame.h)
#define RC422DEV_VERSION "0.2.0"
// Build time "YYYY-MM-DD HH:MM:SS", written on every build by cmake/build_stamp.cmake
extern const char RC422DEV_BUILD_STAMP[];



typedef struct {
    int totalFrames;
    int okFrames;
    int badFrames;
    int bytes;
    int droppedFrames; // complete frames thrown away because the RX task had no free frame object left
    int overflowFrames; // of the BAD frames: how many ended by exceeding HDLC_MAX_FRAME_LENGTH (a missed flag merges two frames)
    int abortedFrames;  // of the BAD frames: how many were cut short by an abort (seven or more ones inside the frame)
} HDLCStats;

     // Initialize values in the struct
extern HDLCStats sentStats;
extern HDLCStats receivedStats;



// Enum for System State: FORWARDING or DEBUG
// Impacts how Console is handled: FOWARD HDLC packets to serial, or have debug console via serial
enum SystemState
{
	FORWARDING,
	DEBUG,
    PACKET_GEN // Generate dummy packets to test serial port receiver
};

extern SystemState systemState;

#endif
