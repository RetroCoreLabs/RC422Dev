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



typedef struct {
    int totalFrames;
    int okFrames;
    int badFrames;
    int bytes;
} HDLCStats;

     // Initialize values in the struct
extern HDLCStats sentStats;
extern HDLCStats receivedStats;

#endif
