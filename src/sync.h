
#ifndef SYNC_H
#define SYNC_H

#include "hdlc.h"

void processPioData(uint8_t data);
void initialize_sync_tasks();
void sendHdlcByte(uint8_t data, bool isFlag);
void inspectCurrentHdlcFrame();
void sendHdlcByte(uint8_t data, bool isFlag);
void SendHDLCFrame(HDLC *frame);

#endif