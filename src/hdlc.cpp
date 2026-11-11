// HDLC protocol based on the "ISO/IEC 13239:2002(E)" specification

// See also https://en.wikipedia.org/wiki/High-Level_Data_Link_Control

// For information on sending PPP in HDLC frames check out RFC-1662 "PPP in HDLC-like Framing"
// https://datatracker.ietf.org/doc/html/rfc1662

#define DEBUG_DETAIL 0

#include <string>
#include "hdlc.h" // also brings in libhdlc/hdlc.h (hdlc_crc_byte, HDLC_FLAG)
#include "pico.h" // __not_in_flash_func: hot-path functions run from SRAM, not from flash through the XIP cache



const char *unknownState = "Unknown state";

// Constructor
HDLC::HDLC(void)
{
    Clear();
}

void __not_in_flash_func(HDLC::Clear)(void)
{
    crc16 = 0xFFFF; // Start with all 1's
    received_crc = 0;
    msg_now = 0;
    msg_received = 0;

    // Zero only the bytes the last frame used (msg_ptr of them), not the whole 1024-byte buffer:
    // Clear runs for every received frame, and nothing reads msg past msg_ptr.
    for (int i = 0; i < msg_ptr; i++)
    {
        msg[i] = 0;
    }
    msg_ptr = 0;
    receiveState = HDLC_STATE_IDLE;
    FrameState = FRAME_EMPTY;
}

hdlc_u_frame_mapping_t HDLC::decode_u_type(uint8_t m1, uint8_t m2, hdlc_connection_state_t connection_state)
{
    // Iterate over the predefined mapping array to find a matching command or response
    for (const auto& mapping : hdlc_u_frame_mappings) {
        // The UNKNOWN sentinel is stored with m1 = m2 = 0, which is also the real code for UI.
        // It must never match a lookup, otherwise UI (control 0x03) decodes as "UNKNOWN".
        if (mapping.type == HDLC_U_MSG_UNKNOWN) {
            continue;
        }
        if (mapping.m1 == m1 && mapping.m2 == m2) {
            // Optionally, add connection_state logic here if necessary for further filtering
            return mapping;
        }
    }
    
    // Return UNKNOWN if no match is found
    return { HDLC_U_MSG_UNKNOWN, 0x00, 0x00, "UNKNOWN", "Unknown frame type" };
}

// Function to decode the HDLC control field
// Returns an hdlc_control_field_t struct with decoded fields from the control byte
hdlc_control_field_t HDLC::GetControlField()
{
    hdlc_control_field_t result;   // Initialize a struct to hold the decoded control field details
    uint8_t control_byte = msg[1]; // Retrieve the control byte from the HDLC message (assumed to be at index 1 of msg array)

    result.txt_msg_type = "UNK";
    result.txt_msg_description = "Unknown frame";
    result.m1 = result.m2 = result.nr = result.ns = 0; // set to 0
    result.frame_type = HDLC_FRAME_TYPE_UNKNOWN;       // Set the frame type to UNKNOWN-frame

    // Decode the Poll/Final (P/F) bit, which is located at bit 4 for all frame types
    result.pf = (control_byte >> 4) & 0x01;

    // Determine frame type by checking the control byte's bit structure
    if ((control_byte & 0x01) == 0)
    { // Check if the least significant bit is 0, indicating an Information (I) frame

        /*
        Information (I) Frames
            Bit 0: 0 indicates an I-frame.
            Bits 1-3: N(S) (Send Sequence Number).
            Bit 4: P/F (Poll/Final) bit.
            Bits 5-7: N(R) (Receive Sequence Number).
        */
        result.frame_type = HDLC_FRAME_TYPE_I; // Set the frame type to I-frame

        // Decode the N(S) field for I-frames, which is the send sequence number
        // N(S) is located in bits 1-3 of the control byte, hence (control_byte >> 1) & 0x07
        result.ns = (control_byte >> 1) & 0x07;

        // Decode the N(R) field for I-frames, which is the receive sequence number
        // N(R) is located in bits 5-7 of the control byte, hence (control_byte >> 5) & 0x07
        result.nr = (control_byte >> 5) & 0x07;

        // Information frames has no "type" text
        result.txt_msg_type = "";
        result.txt_msg_description = "";
    }
    else if ((control_byte & 0x03) == 0x01)
    {

        /*
        Supervisory (S) Frames
            Bits 0-1: 01 indicates an S-frame.
            Bits 2-3: Supervisory Function Code (e.g., RR, RNR, REJ, SREJ).
            Bit 4: P/F (Poll/Final) bit.
            Bits 5-7: N(R) (Receive Sequence Number).
            S-frames do not use N(S).
        */

        // Check if bits 0-1 are 01, indicating a Supervisory (S) frame
        result.frame_type = HDLC_FRAME_TYPE_S; // Set the frame type to S-frame

        result.ns = 0; // S-frames do not use N(S), so we set it to 0

        // Decode the N(R) field for S-frames, which is the receive sequence number
        // N(R) is located in bits 5-7 of the control byte, hence (control_byte >> 5) & 0x07
        result.nr = (control_byte >> 5) & 0x07;

        // Determine the specific S-frame type based on bits 2-3, which indicate supervisory functions
        // We extract bits 2-3 by shifting right by 2 and masking with 0x03
        uint8_t s_type_bits = (control_byte >> 2) & 0x03;
        switch (s_type_bits)
        {
        case 0x00:
            result.s_type = HDLC_S_FRAME_RR;
            break; // Receive Ready (RR) if bits 2-3 are 00
        case 0x01:
            result.s_type = HDLC_S_FRAME_RNR;
            break; // Receive Not Ready (RNR) if bits 2-3 are 01
        case 0x02:
            result.s_type = HDLC_S_FRAME_REJ;
            break; // Reject (REJ) if bits 2-3 are 10 (binary) = 2
        case 0x03:
            result.s_type = HDLC_S_FRAME_SREJ;
            break; // Selective Reject (SREJ) if bits 2-3 are 11 (binary) = 3
        }
        result.txt_msg_type = S_FRAME_TYPE_STRINGS[result.s_type];
        result.txt_msg_description = S_FRAME_DESCRIPTION_STRINGS[result.s_type];
    }
    else if ((control_byte & 0x03) == 0x3) // 11
    {

        /*
        3. Unnumbered (U) Frames
            Bits 0-1: 11 indicates an S-frame.
            Bits 2-3: M1
            Bit 4: P/F (Poll/Final) bit (this is different from I and S frames).
            Remaining Bits: Used to identify specific U-frame types (e.g., SABM, DISC, etc.).
        */

        // Otherwise, it's an Unnumbered (U) frame (typically bits 0-1 are not 00 or 01)
        result.frame_type = HDLC_FRAME_TYPE_U; // Set the frame type to U-frame

        // Unnumbered frame, M1 (Bits [3:2]
        result.m1 = (control_byte >> 2) & 0x03;

        // Unnumbered frame, M2 (Bits [7:5]
        result.m2 = (control_byte >> 5) & 0x07;

        hdlc_u_frame_mapping_t map = decode_u_type(result.m1, result.m2, HDLC_CONNECTION_STATE_IDLE); // for now use IDLE //TODO:FIX state
        result.u_type = map.type;
        result.txt_msg_type = map.msg_name;
        result.txt_msg_description = map.msg_description;
    }

    return result; // Return the struct with the decoded HDLC control field details
}

// Create Control byte for U-Unnumbered frame
uint8_t HDLC::CreateControlU(hdlc_u_frame_type_t u_type, uint8_t poll_final)
{
    uint8_t control_byte = 0;

    // Find the corresponding frame mapping based on the frame type
    for (const auto& mapping : hdlc_u_frame_mappings) {
        if (mapping.type == u_type) {
            uint8_t m1 = mapping.m1;
            uint8_t m2 = mapping.m2;

            // Construct the control byte: bits 0-1 = 11 mark an Unnumbered frame, then M1 and M2.
            // (Without the 0x03 the byte decodes as an I-frame; the first ESP32 test on 2026-11-08
            // caught the UA going out as 0x70 instead of 0x73.)
            control_byte = 0x03 | (m1 << 2) | (m2 << 5);

            // Set the Poll/Final flag if requested
            if (poll_final)
                control_byte |= 1 << 4; // Set Poll/Final flag at bit 4

            return control_byte;
        }
    }
     // Return control byte for UNKNOWN type if frame type not found
    return 0; // Alternatively, return a specific default control byte for unknown frames
   
}

uint8_t HDLC::CreateControlS(hdlc_s_frame_type_t s_type, uint8_t nr, uint8_t poll_final)
{

    uint8_t control_byte = 0;

    // Bits 0-1 = 01 mark a Supervisory frame, bits 2-3 the type, bit 4 P/F, bits 5-7 N(R).
    control_byte = 0x01 | ((s_type & 0x03) << 2);
    control_byte |= (nr & 0x07) << 5;

    // Set the Poll/Final flag if requested
    if (poll_final)
    {
        control_byte |= 1 << 4; // Set Poll/Final flag at bit 4
    }

    return control_byte;
}



void HDLC::DumpControlField(hdlc_control_field_t decoded)
{
    // Display the frame type using string mapping
    printf("Frame Type      : %s\r\n", FRAME_TYPE_STRINGS[decoded.frame_type]);
    printf("Description     : %s\r\n", FRAME_DESCRIPTION_STRINGS[decoded.frame_type]);
    printf("\r\n");
    printf("Msg name        : %s\r\n", decoded.txt_msg_type);        // Decoded short name of message type
    printf("Msg description : %s\r\n", decoded.txt_msg_description); // Descriptive test for the message type

    // Display P/F bit for all frame types
    printf("Poll/Final      : %d\r\n", decoded.pf );

      // Display N(S) only for Information (I) frames
    if (decoded.frame_type == HDLC_FRAME_TYPE_U)
    {
        printf("U:M1            : %d\r\n", decoded.m1);
        printf("U:M2            : %d\r\n", decoded.m2);
    }


    // Display N(S) only for Information (I) frames
    if (decoded.frame_type == HDLC_FRAME_TYPE_I)
    {        
        printf("N(S)            : %d\r\n", decoded.ns);
    }

    // Display N(R) for both Information (I) and Supervisory (S) frames
    if (decoded.frame_type == HDLC_FRAME_TYPE_I || decoded.frame_type == HDLC_FRAME_TYPE_S)
    {
        printf("N(R)            : %d\r\n", decoded.nr);
    }
    printf("------\r\n");
}

// ---- Receive: this object only stores one frame ----
// The bit-level receive work is done by libhdlc's hdlc_decoder in sync.cpp (processPioData): flag
// and abort detection, removing stuffed zeros, the byte-at-a-time fast path, the CRC check and the
// HDLC_MAX_FRAME_LENGTH limit. In its per-frame mode it writes the bytes straight into msg (see
// ReceiveBuffer) and calls ReceivedFrame once at the end of the frame. This replaced the class's
// own bit decoder (ReceiveBit, TryDataByte) on 2026-11-09 when the HDLC code shared with the
// HDLCGW ESP32 firmware moved into libhdlc.
void __not_in_flash_func(HDLC::ReceivedFrame)(int length, FRAME_STATE state)
{
    if (length < 0)
    {
        length = 0;
    }
    if (length > HDLC_MAX_FRAME_LENGTH)
    {
        length = HDLC_MAX_FRAME_LENGTH; // the decoder never writes past the buffer; length agrees
    }
    msg_ptr = (uint16_t)length;
    if (msg_ptr >= 2)
    {
        received_crc = (uint16_t)(msg[msg_ptr - 2] | (msg[msg_ptr - 1] << 8)); // FCS as sent, for DumpFrame
    }
    FrameState = state;
    receiveState = HDLC_STATE_FRAME_COMPLETE;
}

void HDLC::DumpFrame()
{
    int len = msg_ptr - 2; // last 2 is always CRC

    if (receiveState != HDLC_STATE_FRAME_COMPLETE)
    {
        printf("HDLC Receive State = %s\r\n", GetReceiveStateAsText());
        len = msg_ptr;
    }

    // Expected FCS over the frame bytes, for display only (the decoder already judged the frame)
    if (len >= 0)
    {
        uint16_t crc = HDLC_CRC_INIT;
        for (int i = 0; i < len; i++)
        {
            crc = hdlc_crc_byte(crc, msg[i]);
        }
        crc16 = (uint16_t)~crc;
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
int __not_in_flash_func(HDLC::Length)()
{
    return msg_ptr;
}

uint8_t __not_in_flash_func(HDLC::GetFrameByte)(int pos)
{
    if (pos < msg_ptr)
    {
        return msg[pos];
    }

    return 0;
}

HDLCReceiveState __not_in_flash_func(HDLC::GetReceiveState)()
{
    return receiveState;
}

// Function to get string representation of enum value
const char *HDLC::GetReceiveStateAsText()
{
    if (receiveState >= HDLC_STATE_IDLE && receiveState <= HDLC_STATE_FRAME_COMPLETE)
    {
        return HDLCReceiveStateText[receiveState];
    }
    else
    {
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

void HDLC::CreateFrame(const uint8_t *dataArray, size_t length)
{
    crc16 = 0xFFFF;
    Clear();

    for (size_t i = 0; i < length; ++i)
    {
        RawWrite(dataArray[i], true);
    }
    // ADD FCS
    uint16_t crc_copy = crc16 ^ 0xFFFF;
    crc16 = crc_copy;

    received_crc = crc16;

    RawWrite((crc_copy >> 0) & 0xFF, false);
    RawWrite((crc_copy >> 8) & 0xFF, false);

    receiveState = HDLC_STATE_FRAME_COMPLETE;
    FrameState = FRAME_OK;
}


void HDLC::CreateFrameFromByteStuffed(const uint8_t *dataArray, size_t length)
{
    crc16 =0;
    Clear();

    for (size_t i = 0; i < length; ++i)
    {
        RawWrite(dataArray[i], false);
    }    

    receiveState = HDLC_STATE_FRAME_COMPLETE;
    FrameState = FRAME_OK;
}

uint16_t __not_in_flash_func(HDLC::crcIteration)(const uint16_t crc, const uint8_t _b)
{
    return hdlc_crc_byte(crc, _b); // CRC-16/X.25, from libhdlc (was a copy of the RFC 1662 table)
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
