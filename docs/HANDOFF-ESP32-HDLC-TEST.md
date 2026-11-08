# Handoff: Pico <-> ESP32 HDLC line test (2026-11-08)

Next: the rate sweep (5 min at 19200, then 1 min at every rate both boards share, 126-byte
frames both ways). Before it: make BAUD reprogram the PIO transmit clock divider at runtime (see
TODO.md "Baud"), otherwise a BAUD command changes the number but not the line rate.

CLOSED 23:50: the lock interop fix is confirmed, 864/864 and 819/819, zero lag events.

## Setup

- Pico (this repo) wired to an ESP32-S2 Mini running the HDLCGW gateway firmware, 3.3 V GPIO,
  no transceivers: ESP GPIO 11 -> GP16 (data to Pico), ESP GPIO 7 -> GP17 (clock to Pico),
  GP14 -> ESP GPIO 9 (data to ESP), GP15 -> ESP GPIO 12 (clock to ESP), common GND.
  19200 bit/s. The ESP32 has a 25 ns input glitch filter on GPIO 9 and 12 (needed; without it
  single 1->0 bit flips).
- Pico USB-CDC is COM18 on the test PC, Debug Probe attached (flash and RAM reads over SWD with
  OpenOCD from the SDK install). ESP32 is COM10.
- PC tool: `hdlc-loopback` / `hdlc-gateway-tool` from the HdlcGatewayTest repo (sibling repo under
  the RCL tree). `hdlc-loopback --port COM18 --command STAT` identifies the Pico by HELLO and runs a
  command through a control frame. The two-board traffic run is driven from its window (A = COM10,
  B = COM18, 126-byte frames, 3000-byte window each way).
- Two other Claude sessions coordinate this: "HDLCGW" (ESP32 firmware) and
  "hdlc-command-frames-gui" (PC tool). Find them in the agent list.

## Done today (commits c5c4bea and 89ea76d on main, later diagnostics uncommitted)

- Receive decoder rewritten at bit level; the root cause of the old ~50 % loss was the flag-hunt
  register starting at zero (phantom flag after six fill ones). Verified 60/60 both with the ESP's
  old 1..7-ones padding and its new fill. See TODO.md and the commit message of c5c4bea.
- Frame objects from a fixed pool (no heap in the ISR), stuffing counter reset, 0x7E data byte
  stuffed, control-byte markers, REJ/SREJ, UI name, hex parser, 4 bytes of ones fill after every
  closing flag (Ronny's decision), one USB write per frame.
- Host control frames (address 0xFE, un-inverted CRC) so a host never uses the ESC ESC ESC console
  (Ronny's decision). Same format as HDLCGW; verified with the PC tool. CLAUDE.md describes it.
- Pico -> ESP32 direction: 838/838 clean in the last runs.

## Open problem: whole frames lost between the SDK USB write and the USB endpoint

Measured on the 23:15:34 build (uncommitted diagnostics in console_out.cpp, usb_trace.cpp,
sync.cpp fwdLog), 60 s run, 884 frames ESP -> Pico:
- Pico decoded 884/884, handed all 884 to the SDK's `stdio_usb_out_chars` (one write per frame),
  every write returned in under 350 us, DTR never changed, no bus reset.
- Inside TinyUSB (link-time wraps of `usbd_edpt_xfer` and `dcd_event_handler`): endpoint 0x82 IN
  started 133251 bytes = completed 133251 bytes = exactly the PC's raw byte count (+ baseline).
- Bytes handed since baseline = PC bytes + 739, and the five frames missing at the PC
  (sequences 148, 177, 258, 442, 795) have stuffed sizes 144+152+146+144+153 = 739.
- So the frames were handed to the SDK write, which returned fast with no error, and not one of
  their bytes entered a USB transfer. The PC side is cleared (its raw bytes equal its decoded
  frames exactly, 0 driver errors).

FOUND (run 7, 23:41, build 23:37:17): raw bus events ruled out suspend (1 at boot only). Link
wraps on `tud_cdc_n_write` showed TinyUSB was asked for, accepted, started and completed exactly
the PC's byte count; the 13 lost frames were never offered to TinyUSB at all. The lag detector
(`usbLagLog`) named the 13 frames exactly, each with an empty CDC FIFO. Cause, by source:
`pico_sync/mutex.c` `mutex_try_enter_block_until()` returns false AT ONCE when the mutex owner id
equals the caller's ("deadlock, so we can never own it"); with `configSUPPORT_PICO_SYNC_INTEROP 0`
the owner id is the core number, so the RX task writing a frame while the console task holds the
stdio mutex (its 10 ms `getchar` poll, or a control reply) looks like a self-deadlock, the SDK's
`stdio_usb_out_chars` returns on that false, and the whole frame is gone with no counter.

FIX (flashed 23:47:05, confirmed by the 23:50 run: 864/864, 819/819, 0 lag events): `configSUPPORT_PICO_SYNC_INTEROP 1` in
`include/FreeRTOSConfig.h`; the RP2040 FreeRTOS port supports it with one core and the lock owner
becomes the task, so the RX task blocks briefly instead of being refused. Watch the "longest write"
counter afterwards: a frame write can now wait for the console task's poll. HDLCGW's single USB
writer task (one task owning all USB output through a queue) would remove that coupling too; not
done.

## How to read the Pico without the console

- Counters live in RAM; `arm-none-eabi-nm -C build/RC422Dev.elf` gives the addresses of
  `receivedStats`, `sentStats`, `usbBytesRequested`, `inStartBytes`, `inCompleteBytes`, `fwdLog`,
  `fwdLogPos`; read with OpenOCD `mdw`, dump `fwdLog` (1024 x 16 bytes) with `dump_image`.
- `STAT` through a control frame prints the same counters, but sampled before its own reply is
  sent; for exact byte arithmetic read RAM after the host has closed the port.
- The ESC ESC ESC text console only opens before the first forwarded frame; a probe reset
  (`openocd ... -c "init" -c "reset run"`) unlocks it.
