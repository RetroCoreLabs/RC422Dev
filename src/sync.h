
#ifndef SYNC_H
#define SYNC_H

void processPioData(uint8_t data);
void initialize_sync_tasks();
void sendHdlcByte(uint8_t data, bool isFlag);
void inspectCurrentHdlcFrame();

#endif