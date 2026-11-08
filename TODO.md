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
rx_dma_start, tskRxDecode). Not yet measured under traffic.

## Baud

BAUD reprograms the PIO transmit clock divider at runtime (applyTxBaud in src/sync.cpp).
Measured 2026-11-09 against the ESP32: Pico -> line clean at 19200, 57600, 307200 and 983000.
Line -> Pico was clean at 19200 and 57600 but lost frames at 307200 (small frames) and bytes at
983000 with the old per-byte interrupt and one USB write per frame. The DMA receive ring and the
batched USB write (2026-11-09) are meant to fix both; the rerun of the full sweep decides it.