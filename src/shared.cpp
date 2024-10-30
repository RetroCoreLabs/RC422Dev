//#include "shared.h"
//#include "shared.h"
#include <stdio.h>
#include "RingBuffer.h"
// This file contains shared objects and settings across multiple modules

int BUF_SIZE = 4000;  // Define the buffer size for RingBuffer tx and rx

// Define the extern variables from shared.h
RingBuffer* receiveBuffer = nullptr;  // Define the receiveBuffer pointer
RingBuffer* sendBuffer = nullptr;     // Define the sendBuffer pointer

// Global variables for baud rate
uint32_t defaultBaudRate = 19200;
uint32_t maxBaudRate = defaultBaudRate;
uint32_t currentBaudRate = maxBaudRate;


// Loopback flag
bool loopbackEnabled = false;

// Send received data to USB Serial port
bool echoToSerial = true;

// HDLC mode ?
bool hdlcMode = true;




typedef struct {
    int totalFrames;
    int okFrames;
    int badFrames;
    int bytes;
} HDLCStats;

     // Initialize values in the struct
HDLCStats sentStats = {0, 0, 0, 0};
HDLCStats receivedStats = {0, 0, 0, 0};


//HDLC hdlcSendFrame;

/*** INIT CODE ***/


bool InitializeSharedObjects()
{
    receiveBuffer = new RingBuffer(BUF_SIZE);

    if (receiveBuffer == NULL) {
        printf("Unable to allocate receiveBuffer");
        return false;
    }    

    sendBuffer = new RingBuffer(BUF_SIZE);
    if (receiveBuffer == NULL) {
        printf("Unable to allocate sendBuffer");
        return false;
    }    


    return true;
}

// Clean up
void deleteSharedObjects()
{
    if (receiveBuffer != NULL)
    {
        delete receiveBuffer;
        receiveBuffer = NULL;
    }

    if (sendBuffer != NULL)
    {
        delete sendBuffer;
        sendBuffer = NULL;
    }
}