# RetroCore 422 Device (RC422Dev)

RetroCore 422 Device
HW End for RS-422 signals integrating directly with RetroCore app or via RetroCore gateway

Using
  FreeRTOS version: 11.1.0

## What it does

The device is a **bridge between a host PC and a synchronous RS-422 link**. The RP2040's
PIO does the bit-level work — there is no UART on the RS-422 side — so the full data path is:

```text
Host PC  <--- byte-stuffed HDLC over USB CDC --->  RC422Dev  <--- bit-stuffed HDLC over PIO --->  RS-422
             (RFC 1662 escaping, no clock)                         (0x7E flags, bit stuffing, CRC-16)
```

The host speaks framed HDLC over an ordinary COM port; the dongle handles flag detection,
bit stuffing/de-stuffing, clock generation and CRC on the wire side.

### Wire pinout

One channel is currently implemented (the PCB carries three).

| Signal | GPIO | Notes |
|---|---|---|
| TX data | GP14 | to RS-422 driver DI |
| TX clock | GP15 | side-set from PIO |
| RX data | GP16 | from RS-422 receiver RO |
| RX clock | GP17 | |

## Project Structure

```text
/
|
|___/src                    // Main src code directoru
|   
|___/include
|   |___FreeRTOSConfig.h    // FreeRTOS project config file
|
|___/external
|       |
|       \ FreeRTOS-Kernel   // FreeRTOS kernel files, included as a submodule (source: https://github.com/raspberrypi/FreeRTOS-kernel )
|
|
|___.gitmodules             // Definition for Submodules (FreeRTOS)
|___install.txt             // Description on how to manually add submodule
|___CMakeLists.txt          // Top-level project CMake config file
|___pico_sdk_import.cmake   // Raspberry Pi Pico SDK CMake import script
|
|___/docs                   // Reference material (RS-422 transceivers, PIO HDLC design notes, LAPB)
|
|___README.md               // This file
|___LICENSE.md              // MIT License
```

## Operating modes

The USB port is both the data path and the console, so a `SystemState` global decides who
gets the incoming bytes. There are three modes:

| Mode | Purpose | How to enter |
|---|---|---|
| `FORWARDING` | **Default.** Bridges the host and the RS-422 link in both directions. No prompt, no frame dumps. | `EXIT` from `DEBUG`, or three ESCs from `PACKET_GEN` |
| `DEBUG` | Interactive command console (see below). Received frames are pretty-printed instead of forwarded. | Three consecutive ESCs while idle in `FORWARDING` |
| `PACKET_GEN` | Every keystroke becomes an HDLC frame sent to the host — used to exercise a receiver without real traffic. | `PACKETGEN` command from `DEBUG` |

**Known limitation:** the three-ESC escape into `DEBUG` is only checked while no frames have
been forwarded yet (`receivedFrameCount == 0`). Once the link is live, ESC bytes are treated
as frame data and the only way back to `DEBUG` is a reset.

## Console commands

Available in `DEBUG` mode. `HELP` lists them on the target.

| Command | Description |
|---|---|
| `HELP` | List all commands and their parameters |
| `SEND <hex bytes>` | Send a sequence of hex bytes |
| `BAUD <value>` | Set baud rate; `BAUD MAX` for the highest rate |
| `LOOPBACK ON\|OFF` | Enable or disable loopback mode |
| `ECHO` | Echo received data to the serial port |
| `DUMP` | Dump the received buffer |
| `CLEAR` | Clear the received buffer |
| `.` | Send default debug data |
| `HDLC` | Echo HDLC packets |
| `F` | Send an HDLC frame |
| `INSPECT` | Inspect the current HDLC frame |
| `DECODE <hex>` | Decode an HDLC frame from hex values |
| `STAT` | Show HDLC statistics |
| `TASK` | List running FreeRTOS tasks |
| `INFO` | Show OS info |
| `RESET` | Reset the CPU |
| `PACKETGEN` | Switch to packet-generation mode |
| `EXIT` | Leave debug mode, return to `FORWARDING` |

## Usage

Only testet for Windows.

1. Clone the repo: `git clone https://ronnyha@dev.azure.com/ronnyha/RetroCore/_git/RC422Dev`
2. Enter repo: `cd RC422Dev`
3. Prepare the submodules: `git submodule update --init --recursive`
4. Start Visual Studio code: `code .`
5. Install the official [Raspberry Pi Pico extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico)
6. In Visual Studio Code press "F1", then choose "Cmake: Delete Cache and Reconfigure"
7. In Visual Studio Code press "F1", then choose "Cmake: Clean rebuild"
8. Deploy code using UF2 file or "Flash project" using PI Pico Extension.

## Related Projects

- [SyncDongle](https://github.com/MattisLind/alfaskop_emu/tree/master/hardware/SyncDongle) — Mattias Lind's HDLC sync dongle using ESP32 and SPI
- [SDLCBridge](https://github.com/MattisLind/alfaskop_emu/blob/master/Utils/SDLCBridge/README.md) — SDLC bridge utility from the same alfaskop_emu project

## Gotchas

### USB-C "charge-only" cables

The firmware enables `stdio` over USB CDC (`pico_enable_stdio_usb(RC422Dev 1)` in `CMakeLists.txt`), so the target Pico needs to enumerate as a USB serial device on the host PC to see `printf` output and send input back.

Many USB-C cables — especially the ones bundled with phone chargers — are **power-only**: the `VBUS`/`GND` pins are wired but `D+`/`D-` are not. With such a cable the Pico powers up (green LED lights) but no new COM port appears in Device Manager, and the serial terminal has nothing to connect to.

**Symptoms:**
- Pico's power LED is on
- No new `USB Serial Device (COMx)` under Device Manager → Ports (COM & LPT)
- No enumeration event when the cable is plugged/unplugged

**Fix:** Use a known data-capable USB-C cable (any cable that shipped with a USB-C peripheral like an SSD, hub, or dock is a safe bet). Once a proper data cable is connected, the target Pico enumerates as `COMx` and any serial terminal (VS Code Serial Monitor, PuTTY, Tera Term) can open it — baud rate is irrelevant for USB CDC.

### Adding a new source file

`CMakeLists.txt` collects sources with `file(GLOB ...)`, which CMake only evaluates at
configure time. Dropping a new `.cpp`/`.c` into `src/` and running a plain build will
**silently ignore it** — you must reconfigure (F1 → `CMake: Delete Cache and Reconfigure`).
The glob is also non-recursive, so `src/backup/` is not compiled; it is frozen reference
code, not a live path.
