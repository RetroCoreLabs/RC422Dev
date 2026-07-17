# RetroCore 422 Device (RC422Dev)

RetroCore 422 Device
HW End for RS-422 signals integrating directly with RetroCore app or via RetroCore gateway

Using
  FreeRTOS version: 11.1.0

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
|___README.md               // This file
|___LICENSE.md              // MIT License
```

## Usage

Only testet for Windows.

1. Clone the repo https://github.com/RonnyA/FreeRTOS-Pico-Template.git
2. Enter repo: `cd FreeRTOS-Pico-Template`
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