#ifndef HDLC_H
#define HDLC_H

#include <stdint.h>

enum HDLCReceiveState
{
    HDLC_STATE_IDLE,           // Waiting for the start of a frame
    HDLC_STATE_FLAG_DETECTED,  // Detected the HDLC start flag (0x7E)
    HDLC_WAITING_FOR_SYN2,     // CCP/BISYNC
    HDLC_STATE_RECEIVING_DATA, // Receiving the data portion of the frame
    HDLC_STATE_CRC_CALC,       // Calculating/Verifying the CRC
    HDLC_STATE_FRAME_COMPLETE, // Frame fully received. 'FrameState' will say if the frame CRC is valid
};

enum SDLC_PROTOCOL
{
    PROTOCOL_BOP, //		PROTO = 0 BOP (Bit Oriented Protocols: SDLC, HDLC, ADCCP)
    PROTOCOL_CCP, //		PROTO = 1 CCP (Control Character Protocols: BiSync, DDCMP)
};

enum FRAME_STATE
{
    FRAME_EMPTY,
    FRAME_RECEIVING,
    FRAME_OK,
    FRAME_OVERFLOW,
    FRAME_ERROR
};

/************** Control Field *****************/
// Enum for HDLC Frame Types
typedef enum
{
    HDLC_FRAME_TYPE_UNKNOWN, // UNKNOWN
    HDLC_FRAME_TYPE_I,       // Information Frame
    HDLC_FRAME_TYPE_S,       // Supervisory Frame
    HDLC_FRAME_TYPE_U,       // Unnumbered Frame
} hdlc_frame_type_t;

// Enum for Supervisory Frame Types
typedef enum
{
    HDLC_S_FRAME_RR,  // Receive Ready
    HDLC_S_FRAME_RNR, // Receive Not Ready
    HDLC_S_FRAME_REJ, // Reject
    HDLC_S_FRAME_SREJ // Selective Reject
} hdlc_s_frame_type_t;

// Enum for Unnumbered Frame Types with M1 and M2 values in binary

// Need to have HDLC connection state to be able to decode correct RIM,SIM and UI
typedef enum
{
    HDLC_CONNECTION_STATE_IDLE,
    HDLC_CONNECTION_STATE_INITIALIZING,
    HDLC_CONNECTION_STATE_ACTIVE,
    HDLC_CONNECTION_STATE_DISCONNECTED
} hdlc_connection_state_t;

// Enum for Unnumbered Frame Types with Comments for M1 and M2 values
typedef enum
{
    HDLC_U_FRAME_UNKNOWN = 0, // Unknown or Invalid Frame Type, M1 = n/a, M2 = n/a
    HDLC_U_FRAME_SNRM,        // Set Normal Response Mode, M1 = 00, M2 = 001
    HDLC_U_FRAME_SARM,        // Set Asynchronous Response Mode, M1 = 00, M2 = 011
    HDLC_U_FRAME_SABM,        // Set Asynchronous Balanced Mode, M1 = 11, M2 = 100
    HDLC_U_FRAME_DISC,        // Disconnect, M1 = 00, M2 = 010
    HDLC_U_FRAME_SNRME,       // Set Normal Response Mode Extended, M1 = 11, M2 = 001
    HDLC_U_FRAME_SARME,       // Set Asynchronous Response Mode Extended, M1 = 11, M2 = 011
    HDLC_U_FRAME_SABME,       // Set Asynchronous Balanced Mode Extended, M1 = 11, M2 = 110
    HDLC_U_FRAME_SIM,         // Set Initialization Mode, M1 = 00, M2 = 000
    HDLC_U_FRAME_RIM,         // Request Initialization Mode, M1 = 00, M2 = 000
    HDLC_U_FRAME_UI,          // Unnumbered Information, M1 = 00, M2 = 000
    HDLC_U_FRAME_UIH,         // Unnumbered Information with Header Check, M1 = 11, M2 = 010
    HDLC_U_FRAME_UP,          // Unnumbered Poll, M1 = 10, M2 = 000
    HDLC_U_FRAME_UA,          // Unnumbered Acknowledgement, M1 = 00, M2 = 110
    HDLC_U_FRAME_DM,          // Disconnected Mode, M1 = 00, M2 = 100
    HDLC_U_FRAME_FRMR,        // Frame Reject, M1 = 10, M2 = 001
    HDLC_U_FRAME_XID,         // Exchange Identification, M1 = 11, M2 = 101
    HDLC_U_FRAME_RSET,        // Reset, M1 = 11, M2 = 001
    HDLC_U_FRAME_TEST,        // Test, M1 = 11, M2 = 111
    HDLC_U_FRAME_SM           // Set Mode, M1 = 11, M2 = 000
} hdlc_u_frame_type_t;

// Struct to hold decoded control field details
typedef struct
{
    hdlc_frame_type_t frame_type; // Type of HDLC frame (I, S, U)

    uint8_t ns; // Send sequence number (for I-frame)
    uint8_t nr; // Receive sequence number (for I and S-frames)

    uint8_t m1; // Unnumbered frame, M1 (Bits [4:3]
    uint8_t m2; // Unnumbered frame, M2 (Bits [8:6]

    uint8_t pf; // Poll/Final bit

    hdlc_s_frame_type_t s_type; // Supervisory frame type (for S-frames only)
    hdlc_u_frame_type_t u_type; // Unnumbered frame type (for U-frames only)

    const char *txt_msg_type;        // Decoded short name of msg type
    const char *txt_msg_description; // Descriptive test for the msg type
} hdlc_control_field_t;

/************** Control Field *****************/

class HDLC
{

public:
    HDLC();

    // Clear frame/Reset frame
    void Clear();

    // Create a dummy frame for sending (HDLC, binary)
    void CreateDummyFrame();

    // Create a dummy frame for sending (Character Protocol, give your own frame flag)
    void CreateDummyCCPFrame(uint8_t flag);

    void CreateFrame(const uint8_t *dataArray, size_t length);

    // Enable CCP protocol (Bisync) typical the synchar is 0x16
    void EnableCCP(uint8_t synChar);

    // Creating a frame from received data
    void ReceiveByte(const uint8_t data);

    // Receive 1 bit
    void ReceiveBit(uint8_t bit);

    void DumpFrame();

    int Lenght(void);
    uint8_t GetFrameByte(int pos);
    HDLCReceiveState GetReceiveState(void);
    const char *GetReceiveStateAsText(void);

    void RawWrite(const uint8_t data, bool calcCrc);

    FRAME_STATE FrameState;

    hdlc_control_field_t GetControlField();
    void DumpControlField(hdlc_control_field_t decoded);

    uint8_t CreateControlU(hdlc_u_frame_type_t u_type, uint8_t poll_final);

    void ProcessSABMFrame(uint8_t address);

    // Creating a frame for Sending
#ifdef _old_shit_
    void frameOpen();
    void frameWrite(const uint8_t _b);
    uint16_t frameClose();
    uint8_t poll();
    uint8_t getMsg(const uint8_t position);
#endif

    SDLC_PROTOCOL protocol;

private:
    static constexpr uint8_t HDLC_FLAG = (0x7e);
    static constexpr uint8_t HDLC_ESCAPE = (0x7d);
    static constexpr uint8_t HDLC_FLAG_ESC = (0x5e);
    static constexpr uint8_t HDLC_ESCAPE_ESC = (0x5d);
    static constexpr uint8_t HDLC_ESCAPE_MASK = (0x20);
    static constexpr uint16_t HDLC_CRCINIT = (0xffff);
    static constexpr uint16_t HDLC_CRCGOOD = (0xf0b8);
    static constexpr uint16_t HDLC_MAX_FRAME_LENGTH = (128);
    static constexpr uint16_t HDLC_INPUT_BUFFER_SIZE = (HDLC_MAX_FRAME_LENGTH);

    static constexpr uint8_t STX = 0x02; // Start of text
    static constexpr uint8_t ETX = 0x03; // End of text

    inline static uint16_t hdlc_fcstab[256] = {
        0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
        0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
        0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
        0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
        0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
        0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
        0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
        0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
        0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
        0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
        0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
        0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
        0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
        0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
        0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
        0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
        0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
        0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
        0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
        0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
        0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
        0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
        0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
        0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
        0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
        0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
        0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
        0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
        0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
        0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
        0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
        0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78};

    // Matching array of strings for each enum value
    const char *HDLCReceiveStateText[6] =
        {
            "Waiting for the start of a frame",
            "Detected the HDLC start flag (0x7E)",
            "CCP/BISYNC",
            "Receiving the data portion of the frame",
            "Calculating/Verifying the CRC",
            "Frame fully received. 'FrameState' will say if the frame CRC is valid"};

    /***** STRINGS FOR FRAME TYPES ********/
    // String arrays for mapping enums to text
    const char *FRAME_TYPE_STRINGS[4] = {
        "?",
        "I",
        "S",
        "U"};

    const char *FRAME_DESCRIPTION_STRINGS[4] = {
        "Unknown Frame",     //?
        "Information Frame", // I
        "Supervisory Frame", // S
        "Unnumbered Frame"}; // U

    /***** STRINGS FOR SUPERVISORY FRAME TYPES ********/

    // Supervisory frame type
    const char *S_FRAME_TYPE_STRINGS[4] = {
        "RR",
        "RNR",
        "REJ",
        "SREJ"};

    const char *S_FRAME_DESCRIPTION_STRINGS[4] = {
        "Receive Ready",     // RR
        "Receive Not Ready", // RNR
        "Reject",            // REJ
        "Selective Reject"}; // SREJ

    /***** STRINGS FOR UNNUMBERED FRAME TYPES ********/

    // Array of frame type strings
    const char *U_FRAME_TYPE_STRINGS[20] = {
        "UNKNOWN", // HDLC_U_FRAME_UNKNOWN
        "SNRM",    // HDLC_U_FRAME_SNRM
        "SARM",    // HDLC_U_FRAME_SARM
        "SABM",    // HDLC_U_FRAME_SABM
        "DISC",    // HDLC_U_FRAME_DISC
        "SNRME",   // HDLC_U_FRAME_SNRME
        "SARME",   // HDLC_U_FRAME_SARME
        "SABME",   // HDLC_U_FRAME_SABME
        "SIM",     // HDLC_U_FRAME_SIM
        "RIM",     // HDLC_U_FRAME_RIM
        "UI",      // HDLC_U_FRAME_UI
        "UIH",     // HDLC_U_FRAME_UIH
        "UP",      // HDLC_U_FRAME_UP
        "UA",      // HDLC_U_FRAME_UA
        "DM",      // HDLC_U_FRAME_DM
        "FRMR",    // HDLC_U_FRAME_FRMR
        "XID",     // HDLC_U_FRAME_XID
        "RSET",    // HDLC_U_FRAME_RSET
        "TEST",    // HDLC_U_FRAME_TEST
        "SM"       // HDLC_U_FRAME_SM
    };

    // Array of frame description strings
    const char *U_FRAME_DESCRIPTION_STRINGS[20] = {
        "Unknown Frame Type",                       // HDLC_U_FRAME_UNKNOWN
        "Set Normal Response Mode",                 // HDLC_U_FRAME_SNRM
        "Set Asynchronous Response Mode",           // HDLC_U_FRAME_SARM
        "Set Asynchronous Balanced Mode",           // HDLC_U_FRAME_SABM
        "Disconnect",                               // HDLC_U_FRAME_DISC
        "Set Normal Response Mode Extended",        // HDLC_U_FRAME_SNRME
        "Set Asynchronous Response Mode Extended",  // HDLC_U_FRAME_SARME
        "Set Asynchronous Balanced Mode Extended",  // HDLC_U_FRAME_SABME
        "Set Initialization Mode",                  // HDLC_U_FRAME_SIM
        "Request Initialization Mode",              // HDLC_U_FRAME_RIM
        "Unnumbered Information",                   // HDLC_U_FRAME_UI
        "Unnumbered Information with Header Check", // HDLC_U_FRAME_UIH
        "Unnumbered Poll",                          // HDLC_U_FRAME_UP
        "Unnumbered Acknowledgement",               // HDLC_U_FRAME_UA
        "Disconnected Mode",                        // HDLC_U_FRAME_DM
        "Frame Reject",                             // HDLC_U_FRAME_FRMR
        "Exchange Identification",                  // HDLC_U_FRAME_XID
        "Reset",                                    // HDLC_U_FRAME_RSET
        "Test",                                     // HDLC_U_FRAME_TEST
        "Set Mode"                                  // HDLC_U_FRAME_SM
    };

    hdlc_u_frame_type_t decode_u_type(uint8_t m1, uint8_t m2, hdlc_connection_state_t connection_state);
    

    HDLCReceiveState receiveState = HDLC_STATE_IDLE;
    uint16_t crc16;
    uint16_t received_crc;

    uint8_t msg_now, msg_received, msg[HDLC_INPUT_BUFFER_SIZE];

    uint16_t crcIteration(const uint16_t crc, const uint8_t _b);

    uint8_t protocol_flag = HDLC_FLAG;
    int bitCount = 0;
    uint8_t frameByte = 0;
    uint8_t msg_ptr = 0;
    int oneCounter = 0;
    int frameCount = 0;
    bool isFlag = false;

    void AddToFrame(uint8_t data, bool isFlag);
};

#endif
