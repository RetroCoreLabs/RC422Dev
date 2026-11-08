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

Enable DMA for RX data to avoid loosing bits

## Baud

Changing Baud needs to reset clockdiv for PIO program TX