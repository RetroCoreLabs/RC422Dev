// https://github.com/orgua/iLib/blob/main/src/atmel_hdlc.h
#define DEBUG_DETAIL 0

#include <string>

// datasheet: http://tools.ietf.org/html/rfc1662
#include "hdlc.h"

/// HDLC /////////////////////////////////////////////////////////
/* a protocol for serial communication featuring frames, checksum and auto-escaping of characters.


    send:
        Serial.begin(57600);        // open serialport
        hdlc.frameOpen();       // framestart
        hdlc.frameWrite(byte data); //repeat as often as you want
        hdlc.frameClose();      // frame-ending (&checksum) and flush

    receive:
        uint8_t incoming_msg[80];
        if (uint8_t msg_size = hdlc.poll())
        {
            for (uint8_t ivar=0; ivar < size_msg; ++ivar )
            {
                incoming_msg[ivar] = hdlc.getMsg(ivar);
            };
        };
        hdlc.clearMsg();

*/

#if false
String byteToBinaryString(uint8_t byte) {
    String binaryString = "";
    for (int i = 7; i >= 0; i--) {
        binaryString += (byte & (1 << i)) ? '1' : '0';  // Append '1' or '0' based on the bit
    }
    return binaryString;
}
#endif



const char *unknownState = "Unknown state";


// Constructor
HDLC::HDLC(void)
{
    Clear();
}

void HDLC::Clear(void)
{
    protocol = PROTOCOL_BOP;
    protocol_flag = HDLC_FLAG;
    crc16 = 0xFFFF; // Start with all 1's
    received_crc = 0;
    msg_now = 0;
    msg_received = 0;

    bitCount = 0;
    frameByte = 0;
    msg_ptr = 0;
    frameCount = 0;
    isFlag = false;
    receiveState = HDLC_STATE_IDLE;
    FrameState = FRAME_EMPTY;

    for (int i = 0; i < HDLC_INPUT_BUFFER_SIZE; i++)
    {
        msg[i] = 0;
    }
}

void HDLC::EnableCCP(uint8_t synChar)
{
    protocol_flag = synChar;
    protocol = PROTOCOL_CCP;
}

void HDLC::ReceiveByte(uint8_t data)
{

#if DEBUG_DETAIL
    printf("ReceiveByte[0x%02X]\r\n ", data);
#endif
    for (int i = 0; i < 8; i++)
    {
        if (data & 0x80)
        // if (data & 1)
        {
            ReceiveBit(1);
        }
        else
        {
            ReceiveBit(0);
        }
        // data = data >> 1; // rightshift
        data = data << 1; // left
    }
}

// Received a bit thats ONE
// receive LSB first, so shift right 
void HDLC::ReceiveBit(uint8_t bit)
{
    // printf("%d",bit);

    switch (receiveState)
    {
    case HDLC_STATE_IDLE:

        // Trying to find a HDLC flag in the bitstream.. receiving LSB first
        frameByte >>= 1;
        if (bit)
        {
            frameByte |= 0x80;
        }
        frameByte &= 0xFF;

        //printf("\r\nFB=0x%02X\r\n", frameByte);

        if (frameByte == protocol_flag)
        {
#if DEBUG_DETAIL
            printf("found first flag: ");
            printf("0x%02X\r\n ", frameByte);
#endif
            AddToFrame(frameByte, true); // add HDLC flag to frame
            bitCount = 0;
        }
        break;

    case HDLC_STATE_RECEIVING_DATA:
        if ((bit == 0) && (oneCounter == 5))
        {
            // ignore stuffed bit
            // printf("ignore stuffed bit");
            oneCounter = 0;
            return;
        }

        // Receive data bits, LSB first
        frameByte >>= 1;
        if (bit)
        {
            frameByte |= 0x80;
        }
        frameByte &= 0xFF;
        bitCount++;

        if (bit == 1)
        {
            oneCounter++;
        }
        else
        {
            oneCounter = 0;
        }

        if (oneCounter >= 6)
        {
            isFlag = true;
        }

        if (bitCount == 8)
        {
            AddToFrame(frameByte, isFlag);

            // Clear and be ready for next byte
            frameByte = 0;
            bitCount = 0;
            isFlag = false;
        }
        break;

    case HDLC_STATE_FRAME_COMPLETE:
        // Handle completed frame
#if DEBUG_DETAIL
        printf("Frame complete. Found last flag: ");
        printf("0x%02X\r\n ", frameByte);
#endif
        break;
    }
}

// Add 8 bites to the HDLC frame
void HDLC::AddToFrame(uint8_t data, bool isFlag)
{
#if DEBUG_DETAIL
    printf("AddToFrame[0x%02X] Flag[%d] State[%d] \r\n", data, isFlag, receiveState);
#endif

    if (receiveState == HDLC_STATE_FRAME_COMPLETE)
        return;

    switch (receiveState)
    {
    case HDLC_STATE_IDLE:
        if ((data == protocol_flag) && (isFlag))
        {
            crc16 = 0xFFFF;
            msg_ptr = 0;

            if (protocol == PROTOCOL_CCP)
            {
                receiveState = HDLC_WAITING_FOR_SYN2;
            }
            else
            {
                receiveState = HDLC_STATE_RECEIVING_DATA;
            }

            //printf("First flag!\r\n");
            FrameState = FRAME_RECEIVING;
        }
        break;

    case HDLC_STATE_RECEIVING_DATA:

        // Add data to buffer
        if (!isFlag)
        {
            if (msg_ptr < HDLC_MAX_FRAME_LENGTH)
            {
                msg[msg_ptr] = data;

                // Calc CRC with 2 bytes offset as the last two bytes in the frame is the CRC, so when the last FLAG is received we have already calculated the CRC
                if (msg_ptr >= 2)
                {
#if DEBUG_DETAIL
                    printf("[%d]Old CRC[0x%04X] DATA[0x%02X]  ", msg_ptr, crc16, msg[msg_ptr - 2]);
#endif
                    crc16 = crcIteration(crc16, msg[msg_ptr - 2]);
#if DEBUG_DETAIL
                    printf("New CRC[0x%04X]\r\n ", crc16);
#endif
                }

                msg_ptr++;
            }
            else
            {
                // Overflow
                printf("Data buffer overflow\r\n");
                receiveState = HDLC_STATE_FRAME_COMPLETE;
            }
            return;
        }

        if ((data == protocol_flag) && (isFlag))
        {
            if (msg_ptr == 0)
            {
                // Ignore secondary flag's
                // printf("Ignore secondary flag\r\n");
                return;
            }

            if (msg_ptr < 2)
            {
                receiveState = HDLC_STATE_IDLE;
#if DEBUG_DETAIL
                printf("BAD Frame, length = %d\r\n", msg_ptr);
#endif
                msg_ptr = 0;
                return;
            }

            // Check CRC
            received_crc = msg[msg_ptr - 2] | msg[msg_ptr - 1] << 8;
            crc16 = crc16 ^ 0xFFFF;

            if (received_crc == crc16)
            {
                FrameState = FRAME_OK;
            }
            else
            {
                FrameState = FRAME_ERROR;
            }

#if DEBUG_DETAIL
            if (received_crc == crc16)
            {
                printf("Received correct CRC 0x%04X\r\n ", crc16);
            }
            else
            {
                printf("Received wrong CRC 0x%04X  (Correct=0x%04X) \r\n ", received_crc, crc16);
            }
#endif
            // Frame completed
            receiveState = HDLC_STATE_FRAME_COMPLETE;
        }
        break;
    case HDLC_STATE_FRAME_COMPLETE:
#if DEBUG_DETAIL
        printf("Received data after frame is complete");
#endif
        receiveState = HDLC_STATE_IDLE;
        break;
    default:
        break;
    }
}

void HDLC::DumpFrame()
{
    int len = msg_ptr - 2; // last 2 is always CRC



    if (receiveState != HDLC_STATE_FRAME_COMPLETE)
    {
        printf("HDLC Receive State = %s\r\n", GetReceiveStateAsText());
        len = msg_ptr;
    }

    printf("HDLC FrameLength = %d\r\n", len);
    printf("Received CRC=0x%04X", received_crc);
    if (received_crc == crc16)
    {
        printf(" [OK]\r\n");
    }
    else
    {
        printf(" [NOT OK (Correct=0x%04X)] \r\n ", crc16);
    }

    printf("\r\nData:\r\n");
    for (int i = 0; i < len; i++)
    {
        uint8_t data = msg[i];
        printf("0x%02X ", data);
        if ((i + 1) % 8 == 0)
        {
            printf("\r\n");
        }
    }

    printf("\r\n");
}

// Returns the length of the data buffer
// After a full frame the return value is "Data bytes" +2 (as the last 2 bytes is the CRC)
int HDLC::Lenght()
{
    return msg_ptr;
}

uint8_t HDLC::GetFrameByte(int pos)
{
    if (pos < msg_ptr)
    {
        return msg[pos];
    }

    return 0;
}

HDLCReceiveState HDLC::GetReceiveState()
{
    return receiveState;
}

// Function to get string representation of enum value
const char* HDLC::GetReceiveStateAsText() 
{    
    if (receiveState >= HDLC_STATE_IDLE && receiveState <= HDLC_STATE_FRAME_COMPLETE) 
    {
        return HDLCReceiveStateText[receiveState];
    } else {
        return unknownState;
    }
}

void HDLC::RawWrite(const uint8_t data, bool calcCrc)
{
    if (msg_ptr < HDLC_MAX_FRAME_LENGTH)
    {
        msg[msg_ptr++] = data;
        if (calcCrc)
        {
            crc16 = crcIteration(crc16, data);
        }
    }
}

void HDLC::CreateDummyFrame()
{
    crc16 = 0xFFFF;
    Clear();
    RawWrite(HDLC_FLAG, false);
    // RawWriteControl(HDLC_FLAG);

    // RawWriteData(0x55); // Address
    // RawWriteData(0x00); // Control

    for (int i = 0; i < 20; i++)
    {
        RawWrite('A' + i, true);
    }

    // ADD FCS
    uint16_t crc_copy = crc16 ^ 0xFFFF;

    printf("Calculated CRC = 0x%04X\r\n", crc_copy);

    RawWrite((crc_copy >> 0) & 0xFF, false);
    RawWrite((crc_copy >> 8) & 0xFF, false);

    RawWrite(HDLC_FLAG, false);
    RawWrite(HDLC_FLAG, false);
}

void HDLC::CreateNDFrame()
{
    crc16 = 0xFFFF;
    Clear();
    RawWrite(HDLC_FLAG, false);

    RawWrite(0x01, true);    
    RawWrite(0x3F, true);
    RawWrite(0x00, true);
    RawWrite(0x64, true);

    // ADD FCS
    uint16_t crc_copy = crc16 ^ 0xFFFF;
    crc16 = crc_copy;

    received_crc = crc16;

    printf("Calculated CRC = 0x%04X\r\n", crc_copy);

    RawWrite((crc_copy >> 0) & 0xFF,false);
    RawWrite((crc_copy >> 8) & 0xFF,false);

    RawWrite(HDLC_FLAG,false);
    RawWrite(HDLC_FLAG,false);
    DumpFrame();
}

void HDLC::CreateDummyCCPFrame(uint8_t flag)
{
    Clear();
    RawWrite(flag, false);

    RawWrite(0x55, true); // Address
    RawWrite(0x00,true); // Control

    for (int i = 0; i < 20; i++)
    {
        RawWrite('A' + i,true);
    }

    // ADD FCS
    uint16_t crc_copy = ~crc16;

    RawWrite((crc_copy >> 0) & 0xFF,false);
    RawWrite((crc_copy >> 8) & 0xFF,false);

    RawWrite(flag,false);
}

uint16_t HDLC::crcIteration(const uint16_t crc, const uint8_t _b)
{
    return (crc >> 8) ^ hdlc_fcstab[(crc ^ _b) & 0xff];
}

#ifdef _old_shit_
void HDLC::frameOpen()
{
    Serial.write(HDLC_FLAG);
    crc16 = HDLC_CRCINIT;
}

void HDLC::frameWrite(const uint8_t _b)
{
    crc16 = crcIteration(crc16, _b);

    uint8_t bw;

    if (_b == HDLC_FLAG)
    {
        Serial.write(HDLC_ESCAPE);
        bw = HDLC_FLAG_ESC;
    }
    else if (_b == HDLC_ESCAPE)
    {
        Serial.write(HDLC_ESCAPE);
        bw = HDLC_ESCAPE_ESC;
    }
    else
    {
        bw = _b;
    }

    Serial.write(bw);
}

uint16_t HDLC::frameClose()
{
    uint16_t _crc = ~crc16;

    frameWrite((_crc >> 0) & 0xFF);
    frameWrite((_crc >> 8) & 0xFF);

    Serial.write(HDLC_FLAG);
    Serial.flush();
}

uint8_t HDLC::poll()
{
    if (msg_received < 3)
    {
        while (Serial.available())
        {
            uint8_t _b = uint8_t(Serial.read());

            if (msg_received == 0)
            {
                if (_b == HDLC_FLAG)
                    msg_received = 1;
                if (!Serial.available())
                    delay(1);
                continue;
            }
            if ((msg_received == 2) && (_b == HDLC_FLAG))
            {
                msg_received = 3;
                break;
            }
            if (msg_received == 1)
            {
                if (_b == HDLC_FLAG)
                    continue;
                else
                    msg_received = 2;
            }

            if (msg_received == 2)
            {
                if (_b == HDLC_ESCAPE)
                {
                    uint8_t wstop = 0;
                    while (!Serial.available())
                    {
                        delay(1);
                        if (wstop++ > 10)
                            break;
                    }
                    _b = uint8_t(Serial.read());
                    if (_b == HDLC_FLAG_ESC)
                        _b = HDLC_FLAG;
                    if (_b == HDLC_ESCAPE_ESC)
                        _b = HDLC_ESCAPE;
                }
                msg[msg_now] = _b;
                msg_now++;
            }
            if (!Serial.available())
                delay(1);
        }
    }

    if (msg_received == 3)
    {
        msg_now -= 2;

        uint16_t crcT = HDLC_CRCINIT;
        for (uint8_t ivar = 0; ivar < msg_now; ++ivar)
        {
            crcT = crcIteration(crcT, msg[ivar]);
        }
        crcT = ~crcT;
        uint16_t crcM = msg[msg_now] | (msg[msg_now + 1] << 8);

        if (crcM != crcT)
        {
            Clear();
            return 0;
        }

        msg_received = 4;
        return msg_now;
    }

    if (msg_received > 2)
        return msg_now;
    else
        return 0;
}

uint8_t HDLC::getMsg(const uint8_t position)
{
    return (position > (HDLC_INPUT_BUFFER_SIZE - 1)) ? 0 : msg[position];
}
#endif
