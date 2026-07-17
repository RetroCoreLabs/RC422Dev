# RP2040 / RP2350 PIO HDLC Design Discussion

## Overview
This document captures the full technical discussion and design evolution for implementing HDLC (bit-stuffed, synchronous) using PIO state machines.

Focus areas:
- TX design using PIO
- RX design with frame detection and bit de-stuffing
- Efficient DMA-friendly data formats
- Division of responsibility between PIO and C

---

## Key Design Decisions

### 1. Split Responsibilities

**PIO responsibilities:**
- Bit-level timing
- Clock edge alignment
- Bit shifting
- Frame detection (flag: 0x7E)
- Bit de-stuffing

**C responsibilities:**
- Frame parsing
- Protocol (HDLC / Cisco HDLC / LAPB / X.25)
- Error handling
- Buffering

---

## TX Design Analysis

### Current TX Strengths

- Correct use of `side_set` for clock generation
- Deterministic timing using delay slots
- Clean loop using `x` counter

### Issues

1. **Instruction-heavy loop (~33 cycles/bit)**
   - Limits max bitrate
   - Hard to extend (e.g. bit stuffing)

2. **PIO doing too much timing work**

3. **No bit stuffing support in PIO**

---

### Recommended TX Architecture

Move intelligence to C:

- Pre-encode bitstream in C
- Include:
  - Bit stuffing
  - Flags (0x7E)

PIO becomes a **pure shifter**:

```pio
.program tx_shift
.side_set 1

pull block
set x, 31
loop:
    out pins, 1 side 0
    nop        side 1
    jmp x-- loop
```

Advantages:
- Minimal PIO usage
- High speed
- Easy to maintain

---

## RX Design Analysis

### Current RX Strengths

- Correct edge sampling (`wait 0` → `wait 1`)
- Flag detection (0x7E)
- Clean synchronization logic

### Missing Features

- Bit de-stuffing
- Frame boundary signaling
- Error detection (abort: 7+ ones)
- Metadata for C

---

## Critical Improvement: 32-bit RX Format

Instead of pushing raw bytes, use structured words:

```c
typedef union
{
    uint32_t u32;
    struct
    {
        uint32_t data         : 8;   // de-stuffed byte
        uint32_t stuffed_mask : 8;   // per-bit metadata
        uint32_t flags        : 8;   // frame/event flags
        uint32_t aux          : 8;   // reserved
    };
} HdlcRxWord;
```

---

## Metadata Design

### stuffed_mask (bits 15:8)

Each bit corresponds to output bit position:

- `1` → stuffed zero removed before this bit
- `0` → normal bit

### flags (bits 23:16)

| Bit | Meaning |
|-----|--------|
| 16  | Frame start |
| 17  | Frame end |
| 18  | Abort detected |
| 19  | Partial byte |
| 20  | Overflow |

---

## Bit De-stuffing Strategy (PIO)

Use consecutive ones counter:

### Logic

```
if bit == 1:
    ones++
    output bit

if bit == 0:
    if ones == 5:
        // stuffed bit
        discard
        mark metadata
    else:
        output bit
    ones = 0
```

---

## Frame Detection

Flag pattern: `01111110`

### Detection Strategy

- Track run-length of ones
- Detect sequence:
  - 6 ones followed by 0

### Outcomes

| Pattern | Meaning |
|--------|--------|
| 01111110 | Frame flag |
| 01111111 | Abort |

---

## Optimized RX Architecture

### Suggested PIO Structure

Split into logical stages (can be 1–2 SMs):

1. **Edge sampler**
2. **Bit processor**
3. **Byte packer + metadata generator**

---

## DMA Strategy

- Use RX FIFO → DMA → ring buffer
- Each transfer = 32-bit word

Advantages:
- Zero CPU overhead per bit
- Minimal parsing cost

---

## Loopback Mode (Recommended)

Add optional internal loopback:

- TX → RX internally
- Used for:
  - Validation
  - Timing calibration

---

## Cisco HDLC Note

- Bit-level identical to HDLC
- Differences are at protocol layer

Handled entirely in C:

- Address field
- Control field
- Protocol type

---

## Final Architecture Summary

### TX
- PIO = simple shifter
- C = bit stuffing + framing

### RX
- PIO = full bit engine (recommended)
- Output = 32-bit structured words
- C = frame parsing

---

## Key Takeaways

- Do not push raw bytes from PIO
- Always include metadata
- Keep TX simple
- Push complexity to RX or C where appropriate
- Design for DMA from the start

---

## Next Steps

1. Finalize RX PIO implementation with:
   - Bit de-stuffing
   - Metadata generation
2. Validate with loopback
3. Add C-level frame parser
4. Extend to LAPB / X.25

---

End of document

