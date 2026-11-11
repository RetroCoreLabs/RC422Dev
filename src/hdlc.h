#ifndef RC422DEV_HDLC_H
#define RC422DEV_HDLC_H // not HDLC_H: libhdlc/hdlc.h uses that guard

#include <stdint.h>
#include "libhdlc/hdlc.h" // shared HDLC codec: hdlc_decoder, hdlc_encoder, hdlc_crc_byte, HDLC_FLAG

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
    FRAME_ERROR,
    FRAME_ABORT     // cut short by an abort sequence (seven or more ones inside the frame)
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
    HDLC_CONNECTION_STATE_CONNECTED,
    HDLC_CONNECTION_STATE_DISCONNECTED
} hdlc_connection_state_t;

typedef enum
{
    HDLC_U_MSG_UNKNOWN = 0,

    // Commands
    HDLC_U_MSG_CMD_SNRM = 1,
    HDLC_U_MSG_CMD_SARM,
    HDLC_U_MSG_CMD_SABM,
    HDLC_U_MSG_CMD_DISC,
    HDLC_U_MSG_CMD_SNRME,
    HDLC_U_MSG_CMD_SARME,
    HDLC_U_MSG_CMD_SABME,
    HDLC_U_MSG_CMD_SIM,
    HDLC_U_MSG_CMD_UP,
    HDLC_U_MSG_CMD_UI,
    HDLC_U_MSG_CMD_XID,
    HDLC_U_MSG_CMD_RSET,
    HDLC_U_MSG_CMD_TEST,
    HDLC_U_MSG_CMD_SM,
    HDLC_U_MSG_CMD_UIH,

    // Responses
    HDLC_U_MSG_RSP_UA,
    HDLC_U_MSG_RSP_FRMR,
    HDLC_U_MSG_RSP_DM,
    HDLC_U_MSG_RSP_RD,
    HDLC_U_MSG_RSP_RIM,
    HDLC_U_MSG_RSP_UI,
    HDLC_U_MSG_RSP_XID,
    HDLC_U_MSG_RSP_TEST,
    HDLC_U_MSG_RSP_UIH

} hdlc_u_frame_type_t;

typedef struct
{
    hdlc_u_frame_type_t type;
    uint8_t m1;
    uint8_t m2;
    const char *msg_name;
    const char *msg_description;
} hdlc_u_frame_mapping_t;

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
    // Largest frame (data + FCS) this codec holds; frames beyond it end as FRAME_OVERFLOW.
    // Public because the USB forward path sizes its stuffed-output buffer from it.
    // Longest frame the codec holds, address + control + information + 2 FCS bytes.
    // ND XMSG over LAPB uses N1 = 622 bytes of information (XMSG_LAPB_N1 in libxmsg), so a full
    // frame is 622 + 2 + 2 = 626 bytes. Was 128 until 2026-11-09, which cut real XMSG frames.
    // 1024 leaves room above 626. The frame pool holds 34 of these in RAM.
    static constexpr uint16_t HDLC_MAX_FRAME_LENGTH = (1024);
    static constexpr uint16_t HDLC_INPUT_BUFFER_SIZE = (HDLC_MAX_FRAME_LENGTH);

    HDLC();

    // Clear frame/Reset frame
    void Clear();

    void CreateFrame(const uint8_t *dataArray, size_t length);
    void CreateFrameFromByteStuffed(const uint8_t *dataArray, size_t length);

    // Receive: the libhdlc decoder (sync.cpp) writes the frame bytes straight into ReceiveBuffer()
    // (hdlc_decoder_set_buffer, HDLC_MAX_FRAME_LENGTH bytes) and then calls ReceivedFrame once
    // with the length (FCS included) and the outcome. No call per byte.
    uint8_t *ReceiveBuffer() { return msg; }
    void ReceivedFrame(int length, FRAME_STATE state);

    void DumpFrame();

    int Length(void);
    uint8_t GetFrameByte(int pos);
    HDLCReceiveState GetReceiveState(void);
    const char *GetReceiveStateAsText(void);

    void RawWrite(const uint8_t data, bool calcCrc);

    FRAME_STATE FrameState;

    hdlc_control_field_t GetControlField();
    void DumpControlField(hdlc_control_field_t decoded);

    uint8_t CreateControlU(hdlc_u_frame_type_t u_type, uint8_t poll_final);
    uint8_t CreateControlS(hdlc_s_frame_type_t s_type, uint8_t nr, uint8_t poll_final);    

    // Creating a frame for Sending
#ifdef _old_shit_
    void frameOpen();
    void frameWrite(const uint8_t _b);
    uint16_t frameClose();
    uint8_t poll();
    uint8_t getMsg(const uint8_t position);
#endif

    // HDLC_FLAG, HDLC_CRC_INIT and HDLC_CRC_GOOD come from libhdlc/hdlc.h; the RFC 1662 byte
    // stuffing constants from libhdlc/byte_frame.h (BYTE_FRAME_FLAG, BYTE_FRAME_ESCAPE).

private:

    static constexpr uint8_t STX = 0x02; // Start of text
    static constexpr uint8_t ETX = 0x03; // End of text

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

    // Example of initializing the mapping array

    // TODO: Refactor CMD and RSP to have a new flag if its CMD, RSP or both.. See https://en.wikipedia.org/wiki/High-Level_Data_Link_Control
    hdlc_u_frame_mapping_t hdlc_u_frame_mappings[25] = {
        {HDLC_U_MSG_UNKNOWN, 0x00, 0x00, "UNKNOWN", "Unknown frame type"},

        // Commands
        {HDLC_U_MSG_CMD_SNRM, 0x0, 0x4, "SNRM", "Set Normal Response Mode"},  // 	Use 3 bit sequence number (Set mode)
        {HDLC_U_MSG_CMD_SARM, 0x3, 0x0, "SARM", "Set Asynchronous Response Mode"}, // Use 3 bit sequence number (Set mode)
        {HDLC_U_MSG_CMD_SABM, 0x3, 0x1, "SABM", "Set Asynchronous Balanced Mode"}, // Use 3 bit sequence number (Set mode)
        {HDLC_U_MSG_CMD_DISC, 0x0, 0x2, "DISC", "Disconnect"},  // Terminate logical link connection (Future I and S frames return DM)
        {HDLC_U_MSG_CMD_SNRME, 0x3, 0x6, "SNRME", "Set Normal Response Mode Extended"},  // Use 7 bit sequence number (Set mode;extended)
        {HDLC_U_MSG_CMD_SARME, 0x3, 0x2, "SARME", "Set Asynchronous Response Mode Extended"}, // Use 7 bit sequence number (Set mode;extended)
        {HDLC_U_MSG_CMD_SABME, 0x3, 0x3, "SABME", "Set Asynchronous Balanced Mode Extended"},  // Use 7 bit sequence number (Set mode;extended)
        
        {HDLC_U_MSG_CMD_SIM, 0x1, 0x0, "SIM", "Set Initialization Mode"}, // Initialize link control function in the addressed station
        {HDLC_U_MSG_CMD_UP, 0x0, 0x1, "UP", "Unnumbered Poll"},  // Used to solicit control information
        {HDLC_U_MSG_CMD_UI, 0x0, 0x0, "UI", "Unnumbered Information"}, // Unacknowledged data (Has a payload)
        {HDLC_U_MSG_CMD_XID, 0x3, 0x5, "XID", "Exchange Identification"},  // Used to Request/Report capabilities (bot cmd and rsp)
        {HDLC_U_MSG_CMD_RSET, 0x3, 0x4, "RSET", "Reset (combined station only)"}, // Used for recovery (Resets N(R) but not N(S))
        {HDLC_U_MSG_CMD_TEST, 0x0, 0x7, "TEST", "Test Command"}, // 	Exchange identical information fields for testing. (both cmd and rsp)
        {HDLC_U_MSG_CMD_SM, 0x0, 0x6, "SM", "Set Mode"}, // Set mode, generic (New in ISO 13239)
        {HDLC_U_MSG_CMD_UIH, 0x3, 0x7, "UIH", "Unnumbered Information with Header check"},

        // Responses
        {HDLC_U_MSG_RSP_UA, 0x0, 0x3, "UA", "Unnumbered Acknowledge"},  // Acknowledge acceptance of one of the set-mode commands.
        {HDLC_U_MSG_RSP_FRMR, 0x1, 0x4, "FRMR", "Frame Reject"},  // Report receipt of unacceptable frame
        {HDLC_U_MSG_RSP_DM, 0x3, 0x0, "DM", "Disconnected Mode"},  // Responder in disconnected mode (Mode set required)
        {HDLC_U_MSG_RSP_RD, 0x0, 0x2, "RD", "Request Disconnect"},  // 	Solicitation for DISC Command
        {HDLC_U_MSG_RSP_RIM, 0x1, 0x0, "RIM", "Request Initialization Mode"}, // Initialization needed (Request for SIM command)
        {HDLC_U_MSG_RSP_UI, 0x0, 0x0, "UI", "Unnumbered Information Response"},  // Unacknowledged data (Has payload). Is both a "Command And Response" ?
        {HDLC_U_MSG_RSP_XID, 0x3, 0x5, "XID", "Exchange Identification Response"}, // Used to Request/Report capabilities
        {HDLC_U_MSG_RSP_TEST, 0x0, 0x7, "TEST", "Test Response"},  // 	Exchange identical information fields for testing (both cmd and rsp)
        {HDLC_U_MSG_RSP_UIH, 0x3, 0x7, "UIH", "Unnumbered Information with Header check Response"} //Unacknowledged data (New in ISO 13239)  Is both a "Command And Response" ?
        };

    hdlc_u_frame_mapping_t decode_u_type(uint8_t m1, uint8_t m2, hdlc_connection_state_t connection_state);

    HDLCReceiveState receiveState = HDLC_STATE_IDLE;
    uint16_t crc16;
    uint16_t received_crc;

    uint16_t msg_now, msg_received; // only used by the old (not compiled) poll code
    uint8_t msg[HDLC_INPUT_BUFFER_SIZE];

    uint16_t crcIteration(const uint16_t crc, const uint8_t _b);

    uint16_t msg_ptr = 0; // bytes in msg; 16 bits because frames are longer than 255 bytes
};

#endif
