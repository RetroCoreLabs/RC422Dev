#ifndef SHARED_H
#define SHARED_H

#include <stdio.h>
#include "RingBuffer.h"
#include "hdlc.h"

bool InitializeSharedObjects();

extern RingBuffer* receiveBuffer;
extern RingBuffer* sendBuffer;

extern bool receiveToSerialEnabled;

extern bool loopbackEnabled;
extern bool receiveToSerialEnabled;
extern bool hdlcMode;

extern uint32_t defaultBaudRate;
extern uint32_t currentBaudRate;
extern uint32_t maxBaudRate;

// Loopback flag
extern bool loopbackEnabled;

// Send received data to USB Serial port
extern bool receiveToSerialEnabled;

// HDLC mode ?
extern bool hdlcMode;


#endif
