# Requirements for the RC422Dev firmware

Each requirement below was either decided by Ronny or forced by a measured failure. Every one
names where it is implemented and how it was verified. "Decided" items are not to be re-opened
without Ronny; "measured" items may be revisited only with a new measurement that contradicts
the old one. Dates are 2026-11-08 unless stated.

## 1. HDLC line side (PIO, GP14..GP17)

**R1.1 Bit-level HDLC receiver (ISO 13239).** The receiver works on bits, not bytes:
- a flag (01111110) is recognised wherever it falls, not only on the frame's byte boundary;
- a zero after five ones is a stuffed bit and is dropped;
- seven or more ones inside a frame are an abort; the frame is dropped and counted;
- fewer than 32 bits between two flags is not a frame and is ignored silently;
- one flag may close a frame and open the next;
- the flag-hunt register idles at all ones, never zero.
Why: with a byte-aligned flag check and a zero-initialised hunt register, six fill ones followed
by a real flag looked like a flag one bit early, the next frame was eaten, and about half of all
back-to-back frames were lost (measured on the raw PIO byte stream). Where: `src/hdlc.cpp`
`ReceiveBit`, `Clear`, `BeginFrameAfterFlag`. Verified: 60/60 frames back to back with 1..7 fill
ones between flags, and 884/884 in every 60 s traffic run since.

**R1.2 Every data byte is bit-stuffed, including 0x7E.** A data byte equal to the flag is never
sent raw. Why: it split frames at the receiver (measured, ESP32 raw dump). Where: `src/sync.cpp`
`SendHDLCFrame`. Verified: probe frame `01 03 FF FF FF FF 7E 7D 1F FF` decoded intact.

**R1.3 Stuffing counters restart at every flag**, both transmit (`ones`) and receive
(`oneCounter`), and `Clear()` resets the receive counter. Why: a stale count swallowed a real
zero of the next frame. Where: `src/sync.cpp` `sendHdlcByte`, `src/hdlc.cpp`.

**R1.4 Inter-frame fill: at least 4 bytes of ones after every closing flag, then the clock may
stop (decided by Ronny).** Why: the ESP32's SPI receive DMA delivers a 4-byte word only after a
later byte arrives, so without fill the closing flag stayed stuck until the next frame. Where:
`src/sync.cpp` `HDLC_TX_FILL_BYTES`. Verified: single frames decode on the ESP32 within a second.

**R1.5 Control bytes carry their frame-type marker bits**: U-frames end in binary 11 (0x03),
S-frames in 01. REJ/SREJ decode from the 2-bit field, UI decodes by name. Why: UA went out as
0x70 instead of 0x73. Where: `src/hdlc.cpp` `CreateControlU`, `CreateControlS`, `decode_u_type`.

**R1.6 Transmit timing (documented fact, not to be changed without a reason):** data changes at
the falling clock edge and is stable 25 us before and 27 us after the rising edge at 19200 bit/s
(33 PIO cycles per bit). A receiver samples on the rising edge. Where: `src/sdlc.pio`.

## 2. Interrupt and task rules

**R2.1 No heap, no printf, no lock in the PIO receive interrupt.** Frame objects come from a
fixed pool exchanged with `xQueueReceiveFromISR` / `xQueueSendFromISR`; a dry pool drops the frame
and counts it (`DROPPED`). Why: `new`/`delete` went through the SDK malloc mutex from the
interrupt and raced the task's `delete`. Where: `src/sync.cpp` `queueHDLCFrame`,
`releaseHDLCFrame`.

**R2.2 `configSUPPORT_PICO_SYNC_INTEROP` must be 1.** Why: every Pico SDK lock refuses a caller
it believes already owns it (`mutex_try_enter_block_until` returns false at once); without the
interop the owner id is the CPU core, so under FreeRTOS every task is "the same owner". The USB
stdio driver uses that call and silently discards the whole write on a refusal: about 1 in 100
frames forwarded to the USB host vanished, exactly the frames written while the console task's
`getchar` poll held the stdio mutex. Proven by byte accounting across TinyUSB (link-time wraps)
and a per-write lag log naming every lost frame. Where: `include/FreeRTOSConfig.h`. Verified:
the first 60 s two-board run after the change was 864/864 and 819/819 with zero lag events;
the longest USB write rose from 342 us to 1278 us, which is the RX task now waiting for the
console task's poll instead of being refused.

**R2.3 Binary frames to the USB host go through `console_out_usb_write()` only, never through
`printf` or `stdio_put_string`**, and one frame is one write. Why: byte-by-byte writes contended
the stdio mutex ~260 times per frame and the driver drops without feedback; printf may at any
moment be redirected into a control-frame reply capture. Where: `src/console_out.cpp`,
`src/sync.cpp` `SendByteStuffed`.

**R2.4 The receive FIFO must never overflow unnoticed.** The PIO RX FIFO overflow flag (FDEBUG
RXSTALL) is the check; it stayed clear in every run. Any future rate increase is to be measured
against it before claiming it works.

## 3. USB host protocol

**R3.1 A host never needs the ESC ESC ESC text console (decided by Ronny).** It uses control
frames: an ordinary byte-stuffed frame with address 0xFE whose two check bytes are the CRC-16/X.25
register stored un-inverted (residue 0x0000; a good data frame leaves 0xF0B8, so the two never
collide). The firmware answers HELLO, COMMAND (output as REPLY chunks of at most 122 bytes, LAST
flag on the final chunk), LOG_LEVEL, STAT_PUSH, PING; unknown types get a NAK. A control frame
never goes to the line and never counts as a forwarded frame (so the console lock cannot trigger
from it). Format identical to the HDLCGW ESP32 firmware and `ControlFrame.cs` in the
HdlcGatewayTest repo, byte for byte; `control_frame.{cpp,h}` must stay in step with them. Where:
`src/control_frame.{cpp,h}`, `src/console_out.{cpp,h}`. Verified: `hdlc-loopback --port COMn
--command STAT --command "BAUD 19200"` identifies "RC422Dev 0.2.0, control frames" and gets both
replies framed.

**R3.2 STAT frame fields are append-only.** Order: sentTotal, sentOk, sentBad, sentBytes,
recvTotal, recvOk, recvBad, recvBytes, then this firmware's own fields in the order
`console_out_stat_push` writes them. Never reorder or remove; the PC names them by position.

**R3.3 The text console stays for humans** and still opens with three ESC bytes before the first
forwarded frame; the three-ESC lock after that is unchanged.

**R3.4 PACKETGEN is refused through a control frame** (it would stop control-frame parsing).

## 4. Frame size

**R4.1 Maximum frame 128 bytes including FCS** (`HDLC::HDLC_MAX_FRAME_LENGTH`); larger frames end
as `FRAME_OVERFLOW` and are counted. The HELLO reply states `maxframe=128` so hosts size their
test frames (126 bytes with FCS in the two-board test).

## 5. Verification record

- ESP32 to Pico, 60 s runs, 884 frames each: before R2.2, 871..879 reached the PC; the lost ones
  were named by the lag log and matched the PC's list exactly.
- Pico to ESP32: 838/838 once the ESP32 filtered 25 ns glitches on its receive pins; before that
  about 1 in 300 frames had a single bit read 1 as 0 on the ESP32 side (not a Pico fault; the Pico's
  timing per R1.6 leaves 25 us of margin).
- The real Norsk Data machine has NOT been re-tested since these changes.
