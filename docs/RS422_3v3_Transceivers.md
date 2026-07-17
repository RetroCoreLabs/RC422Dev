# RS‑422/RS‑485 Transceivers for 3.3 V Systems (RP2040 / Pi Pico)

This document lists **3.3 V‑native** RS‑422/RS‑485 parts suitable for a Raspberry Pi Pico (RP2040). Focus: **two outbound signals (TX Data, TX Clock) + two inbound signals (RX Data, RX Clock)**, up to **1 Mbit/s**, with emphasis on **low cost** and **noise robustness**.

---

## Quick selection

- **Simplest wiring (full‑duplex, robust):** **2 × MAX3490E** (one device for Data, one for Clock). No DE/RE control required.
- **Lowest cost per channel:** **AM26LV31E (quad driver) + AM26LV32E (quad receiver)**. One of each covers TXD, TXC, RXD, RXC (two spare channels remain).

> Use **twisted pairs** and **120 Ω termination** at the receiving end of each direction. Add **TVS** at the connector for surge/ESD (e.g., SM712 class).

---

## Core comparison (families)

| Part family | Function / Duplex | VCC | Channels/IC | Max data rate (typ.) | DE/RE pins | Fail‑safe Rx | ESD note | Typical packages | Pros | Cons |
|---|---|---:|---:|---:|---|---|---|---|---|---|
| **MAX3490E** | Transceiver, **full‑duplex** | 3.3 V | 1 Tx + 1 Rx | ~**10 Mb/s** | **No** | Yes | “E” = enhanced ESD | SOIC‑8, PDIP‑8 | Easiest drop‑in full‑duplex; good noise margin | One channel per IC → need 2 for Data+Clock |
| **MAX3488** | Transceiver, **full‑duplex** | 3.3 V | 1 Tx + 1 Rx | **0.25 Mb/s** | **No** | Yes | Slew‑rate limited | SOIC‑8, PDIP‑8 | Very low EMI | **Too slow** for 1 Mbit/s |
| **MAX3485(E)** | Transceiver, **half‑duplex** | 3.3 V | 1 shared | ~**10–12 Mb/s** | **Yes** | Yes | “E” = enhanced ESD | SOIC‑8, PDIP‑8 | Cheap, fast half‑duplex | Not full‑duplex; firmware must toggle DE/RE |
| **MAX3486(E)** | Transceiver, **half‑duplex** | 3.3 V | 1 shared | **2.5 Mb/s** | **Yes** | Yes | Slew‑limited | SOIC‑8, PDIP‑8 | Lower EMI than 3485 | Slower; not full‑duplex |
| **AM26LV31E** | **Quad driver** (TX only) | 3.0–3.6 V | **4 drivers** | **≥ 32 Mb/s** class | Per‑channel enable | — | Enhanced ESD on “E” | SOIC‑16, PDIP‑16 | **Lowest BOM/chan**; covers TXD+TXC (+2 spare) | Needs separate receivers |
| **AM26LV32E** | **Quad receiver** (RX only) | 3.0–3.6 V | **4 receivers** | **≥ 32 Mb/s** class | — | **Open‑input fail‑safe** | Enhanced ESD on “E” | SOIC‑16, PDIP‑16 | Pairs with 31E to complete link | — |
| **ADM3485E** | Transceiver, **half‑duplex** | 3.0–3.6 V | 1 shared | ~**10 Mb/s** | **Yes** | Yes | ±15 kV class | SOIC‑8, PDIP‑8 | Proven drop‑in half‑duplex | Not full‑duplex |

> Notes: Data‑rate figures are family‑typical. Verify the exact orderable code and package in the vendor datasheet for final limits and ESD ratings.

---

## Variant matrices (orderable options)

### MAX3490 — Full‑duplex, ~10 Mb/s, 3.3 V

| Variant | Package | Duplex | DE/RE control | ESD / Grade | Comment |
|---|---|---|---|---|---|
| **MAX3490SA** | SOIC‑8 | Full | **No** | Standard | SMD baseline |
| **MAX3490EESA** | SOIC‑8 | Full | **No** | **Enhanced ESD** | **Recommended** SMD |
| **MAX3490PA** | PDIP‑8 | Full | **No** | Standard | Through‑hole |
| **MAX3490EEPA** | PDIP‑8 | Full | **No** | **Enhanced ESD** | **Recommended** DIP |

**Pin polarity (MAX34xx convention):** **Y = TX+ (non‑inverting)**, **Z = TX− (inverting)**, **A = RX+ (non‑inverting)**, **B = RX− (inverting)**.

---

### MAX3488 — Full‑duplex, **0.25 Mb/s** (slew‑rate limited), 3.3 V

| Variant | Package | Duplex | DE/RE | Comment |
|---|---|---|---|---|
| **MAX3488SA / MAX3488PA** | SOIC‑8 / PDIP‑8 | Full | **No** | Very low EMI; **not suitable** for ≥ 1 Mb/s |

---

### MAX3485 / MAX3486 — Half‑duplex, 3.3 V

| Part | Variant | Package | Max rate (typ.) | DE/RE | Comment |
|---|---|---|---:|---|---|
| **MAX3485 / MAX3485E** | SA / PA | SOIC‑8 / PDIP‑8 | ~**10–12 Mb/s** | **Yes** | Fast 2‑wire bus; enable control required |
| **MAX3486 / MAX3486E** | SA / PA | SOIC‑8 / PDIP‑8 | **2.5 Mb/s** | **Yes** | Slew‑limited (lower EMI), slower |

> Use these only if your link is **half‑duplex**. For true RS‑422 (separate TX± and RX±), prefer full‑duplex parts (e.g., MAX3490E) or the driver/receiver pair below.

---

### AM26LV31E (TI) — **Quad RS‑422 Driver** (TX only), 3.0–3.6 V

| Package code | Package | Channels | Enable pins | Notes |
|---|---|---:|---|---|
| **DW** | SOIC‑16 | 4 | Per‑channel | Cost‑effective; plenty of speed headroom |
| **N** | PDIP‑16 | 4 | Per‑channel | Easy prototyping |

**Outputs:** Non‑inverting **Y (TX+)** and inverting **Z (TX−)** per channel (per datasheet naming).

---

### AM26LV32E (TI) — **Quad RS‑422 Receiver** (RX only), 3.0–3.6 V

| Package code | Package | Channels | Fail‑safe | Notes |
|---|---|---:|---|---|
| **DW** | SOIC‑16 | 4 | Open‑input | Pairs with AM26LV31E |
| **N** | PDIP‑16 | 4 | Open‑input | Easy prototyping |

**Inputs:** Non‑inverting **A (RX+)** and inverting **B (RX−)** per channel (per datasheet naming).

---

### ADM3485E (Analog Devices) — Half‑duplex transceiver, 3.0–3.6 V

| Variant | Package | Rate | DE/RE | Notes |
|---|---|---:|---|---|
| **ADM3485E** | SOIC‑8 / PDIP‑8 (vendor‑dependent) | ~**10 Mb/s** | **Yes** | Robust 2‑wire bus option (not full‑duplex) |

---

## Wiring & layout guidance (applies to all)

- **Polarity:** Maintain **TX+ ↔ RX+** and **TX− ↔ RX−** end‑to‑end. For MAX34xx: **Y=TX+**, **Z=TX−**, **A=RX+**, **B=RX−**.
- **Termination:** Place **100–120 Ω** across each pair **at the receiving end** of that direction (i.e., far end for TX pairs; local end for RX pairs).
- **Biasing / idle:** Prefer fail‑safe receivers; if the far end may float, use weak bias resistors to define idle.
- **Protection:** Add a **differential TVS** (e.g., SM712‑class) per pair at the connector. Keep TVS and return path short.
- **Edge control:** If ringing is observed, add **10–33 Ω** series resistors at driver outputs.
- **Cabling:** Use **twisted pairs**; keep stubs very short; length‑match Data and Clock pairs as practical.
- **Grounding/shield:** Tie cable shield to chassis (one end or as per EMC plan). Keep high‑di/dt currents off logic ground.

---

## Selection for 1 Mbit/s with TX/RX Data & Clock

- **Prefer simplicity & robustness:** **2 × MAX3490E** (Data + Clock).  
- **Prefer minimum cost per channel:** **AM26LV31E + AM26LV32E** (covers all four signals on two ICs).  
- **Avoid for this use:** **MAX3488** (0.25 Mb/s), half‑duplex parts (**MAX3485/3486**, **ADM3485E**) unless your topology is 2‑wire shared‑bus.

---

## Ordering checklist

1. Choose **full‑duplex solution**:  
   - **MAX3490EESA/EEPA** × **2** (Data, Clock) **OR** one **AM26LV31E** + one **AM26LV32E**.
2. Add **connectivity**: cable/connector matching your system; twisted pairs.
3. Add **termination**: 120 Ω at each receiving end.
4. Add **protection**: TVS per pair (e.g., SM712 class).
5. Layout per guidance above; confirm polarity before fab.

