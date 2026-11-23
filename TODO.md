# TODO

Challenges:
    Receiving from a real ND machine has not been re-tested since 2026-11-08. Before that date the
    receiver lost about half of all back-to-back frames; that was NOT a speed problem. Measured on
    the raw PIO byte stream against an ESP32 at 19200 bit/s: the PIO RX FIFO never overflowed, and
    the loss came from the frame decoder (flag-hunt register started at zero, so six fill ones plus
    a real flag's leading zero looked like a flag one bit early; plus heap use in the interrupt,
    stale stuffing counter, data byte 0x7E sent unstuffed). All fixed in src/hdlc.cpp and
    src/sync.cpp; 60/60 frames both ways at 19200 against the ESP32. The ND machine run must be
    repeated to see whether anything remains there.

## SMP

Enable SMP and lock RX to its own CORE

## DMA

Done 2026-11-09: DMA copies the PIO receive FIFO into an 8 KB ring, decoded by a task (sync.cpp
rx_dma_start, tskRxDecode). Measured 2026-11-11 at 983000 bit/s: no ring overrun, no frame dropped.

## Baud

BAUD reprograms the PIO transmit clock divider at runtime (applyTxBaud in src/sync.cpp).
Measured 2026-11-09 against the ESP32: Pico -> line clean at 19200, 57600, 307200 and 983000.
Line -> Pico was clean at 19200 and 57600 but lost frames at 307200 (small frames) and bytes at
983000 with the old per-byte interrupt and one USB write per frame. The DMA receive ring and the
batched USB write (2026-11-09) fixed both: the sweeps of 2026-11-09 and 2026-11-11 (19200 to 983000,
10 to 626-byte frames) are clean. BAUD is now saved in flash (src/settings.cpp) and restored at start-up.
The 60 s one-way Pico -> ESP32 run at 307200 with 626-byte frames (2026-11-16) was clean: 3156 sent,
3156 ok, 0 bad, 0 lost; the extra bad frame seen earlier does not occur any more. Frames up to 4105
octets pass both ways (see docs/REQUIREMENTS.md).
Tested against the ND-100 emulator (D100) on 2026-11-23: XMSGGW on the ESP32 <-> line <-> Pico <-> PC
window bridge <-> D100 at 307200: telnet login, CONNECT D100 with SINTRAN login and commands, FTP
listings and a 46,516-byte download in 13 s. Pico STAT afterwards: SENT 5765 ok, RECEIVED 9155 ok, 0 bad,
0 dropped, queue at most 3 of 10, DMA overruns 0. Not tested: a real ND hardware machine.