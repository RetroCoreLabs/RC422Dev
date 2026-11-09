# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Platform

Embedded firmware for the **Raspberry Pi Pico (RP2040, Cortex-M0+)** built with the **Pico SDK 2.3.0** and **GCC ARM 13_2_Rel1** (the versions set in `CMakeLists.txt`). RTOS is **FreeRTOS 11.1.0** using the Raspberry Pi fork of `FreeRTOS-Kernel` (submodule at `external/FreeRTOS-Kernel`), port `portable/ThirdParty/GCC/RP2040` (the Raspberry Pi RP2040 port; NOT the generic `GCC_ARM_CM0` port, see the comment in `CMakeLists.txt`), heap_4. `configNUMBER_OF_CORES` is 1 and **`configSUPPORT_PICO_SYNC_INTEROP` MUST stay 1** (set 2026-11-08). Lesson, paid for with a day of measuring: every Pico SDK lock refuses a caller it believes is itself (`mutex_try_enter_block_until` returns false at once, "deadlock, so we can never own it"), and without the interop the owner id is the CORE number, so under FreeRTOS every task on the core is "itself". The USB stdio driver uses exactly that call and then silently throws the whole write away; about 1 in 100 frames forwarded to the host vanished whenever the console task's `getchar` poll held the stdio mutex at the moment the RX task wrote a frame. With the interop on, the owner is the task and the caller blocks instead. Corollary: frame data must never go through a `printf` path, and USB output stays in `console_out_usb_write()`. CMake build type defaults to `Debug` when none is given, and is deliberately not forced: `-DCMAKE_BUILD_TYPE=Release` on the command line works. Keep it that way.

Target board is `pico` (RP2040); the CMakeLists has conditional scaffolding for RP2350 variants, but that path is untested here.

## Build, flash, debug

All paths assume Windows with the Pico SDK installed via the official installer under `%USERPROFILE%\.pico-sdk\`. Use **PowerShell or cmd**, not bash, on Windows — this repo is developed on Windows only.

- **Build (from VS Code):** `Ctrl+Shift+B` runs the `Compile Project` task = `ninja -C build`.
- **Build (from shell):** `cmake -S . -B build -G Ninja` then `ninja -C build`. Build artifacts land in `build/`: `RC422Dev.elf` (for gdb), `RC422Dev.uf2` (BOOTSEL drag-drop), `RC422Dev.bin`, `RC422Dev.dis`, `RC422Dev.elf.map`.
- **Flash with the probe (OpenOCD), stop the DMA and PIO first.** The receive DMA channel keeps copying line bytes into RAM after a probe `reset halt` (that resets the cores, not the DMA or PIO; the firmware's own start-up code does that), and the line clock from the peer keeps it busy. Since the program runs from SRAM (`copy_to_ram`), that ring overlaps OpenOCD's flash work area and corrupts the image being written: "Verify Failed", flash full of 0xFF patches (2026-11-09). Use:
  `openocd -s scripts -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "adapter speed 5000" -c "init" -c "reset halt" -c "mww 0x50200000 0" -c "mww 0x50000444 0xfff" -c "program build/RC422Dev.elf verify reset exit"` (PIO0 CTRL = 0 stops the state machines, DMA CHAN_ABORT = 0xfff stops every channel). Never accept a flash without "Verified OK".
- **Do not leave a core debug-halted.** The RP2040 timer pauses while EITHER core is halted by the debugger (TIMER DBGPAUSE). `halt` stops both cores and `resume` only restarts core 0; core 1 (unused under FreeRTOS) stays halted, `time_us_32()` freezes, and every time-based STAT number reads 0 while the FreeRTOS tick (SysTick) carries on. Read registers with `init` + `mdw` (no halt); if you halted, reset the board.
- **Flash without probe:** drag `build/RC422Dev.uf2` onto the BOOTSEL mass-storage drive, or run the `Run Project` task (picotool `load -fx`).
- **Debug:** select **"Pico Debug (Cortex-Debug)"** in VS Code's Run and Debug dropdown, then `F5`. The launch config drives OpenOCD + CMSIS-DAP, halts at `main`, loads the ELF. Requires the Cortex-Debug VS Code extension and the Raspberry Pi Pico VS Code extension (the `${command:raspberry-pi-pico.*}` variables in `launch.json` are provided by the latter). The hardware probe is a **Raspberry Pi Debug Probe**, which enumerates as CMSIS-DAP — no config changes needed for that. RP2040 gives **4 hardware breakpoints max**.
- **Serial stdio:** `CMakeLists.txt` enables `pico_enable_stdio_usb` and disables `pico_enable_stdio_uart`. Stdio comes out of the **target Pico's own USB port**, not the debug probe's UART. Requires a *data-capable* USB-C cable (power-only cables are a real trap — see README "Gotchas"). Baud rate is irrelevant for USB CDC.

### Adding files — READ BEFORE ADDING FILES

`CMakeLists.txt` lists the sources by name in `set(SOURCES ...)`. **A new `.cpp`/`.c` in `src/` is not built until you add it to that list**; the link fails with an unresolved symbol if you forget. (An earlier version used `file(GLOB ...)`, which only runs at configure time and silently ignored new files.) `src/backup/` is not in the list on purpose.

### PIO header generation

`src/sdlc.pio` is compiled by `pico_generate_pio_header()` into `build/sdlc.pio.h` at build time. Never hand-edit that header — edit the `.pio` source and rebuild. Any change to `sdlc.pio` triggers regeneration automatically.

## Architecture

### Boot sequence (`src/main.cpp`)

```
stdio_init_all()
  → InitializeSharedObjects()     // allocate ring buffers, stats, globals
  → initialize_sync_tasks()       // launches tskSyncRX + tskSyncTX (PIO bit I/O + HDLC)
  → initialize_console_task()     // launches tskConsole (USB-CDC command line)
  → vTaskStartScheduler()
```

`vApplicationStackOverflowHook` in `main.cpp` halts with `taskDISABLE_INTERRUPTS()` and spins — the default way to catch overflow under the debugger.

### Task layout (FreeRTOS, 125 MHz CPU clock, 1 kHz tick, 5 priority levels)

Created in `src/sync.cpp` and `src/console.cpp`:

| Task | Priority | Role |
|---|---|---|
| `RxDecode` | `tskIDLE_PRIORITY + 3` | Every 1 ms, decodes all bytes the DMA has copied from the PIO receiver into the 8 KB receive ring (`rxDmaRing`), bit by bit through `HDLC::ReceiveBit` (all-ones idle bytes take a fast path), posts complete frames to `hdlcQueue` |
| `tskSyncRX` | `tskIDLE_PRIORITY + 2` | Takes complete frames from `hdlcQueue` and forwards every frame waiting there to the USB host in ONE write |
| `tskSyncTX` | `tskIDLE_PRIORITY + 2` | Serializes outgoing bytes via PIO TX |
| `tskConsole` | set in `console.cpp` | Command-line over USB-CDC; also drains `receiveBuffer`, echoes/loops-back, forwards to HDLC decoder when `hdlcMode` is set |

`hdlcQueue` (FreeRTOS `QueueHandle_t`) carries `HDLC*` pointers from the decode task to the RX task. Frame objects come from a fixed static pool (`framePool[]`, `FRAME_POOL_SIZE = MAX_FRAME_QUEUE_SIZE + 2`), never from the heap: the decode task takes a fresh object from `freeFrameQueue` without blocking, the RX task gives it back with `releaseHDLCFrame()`. If the pool runs dry the frame is dropped and `receivedStats.droppedFrames` counts it (shown by `STAT`). The decode task must never block or print: if it falls behind by more than the ring, bytes are lost (`STAT` "RX DMA ring ... overruns"). **Never put `new`/`delete`/`malloc`/`printf` in any interrupt handler** (the PIO TX-FIFO-has-room handler, the USB chars-available callback) — on the Pico SDK those take the malloc mutex and the stdio lock.

### Data flow and shared state (`src/shared.{h,cpp}`)

The system has three `SystemState` modes defined in `shared.h`:
- `FORWARDING` — default, HDLC packets flow through
- `DEBUG` — serial console gets the USB-CDC port for interactive commands
- `PACKET_GEN` — generates dummy packets to stress-test the line

Runtime flags (global `extern bool`):
- `echoToSerial` — echo raw received bytes to USB CDC
- `loopbackEnabled` — copy RX bytes into `sendBuffer` (loop-back for bring-up testing)
- `hdlcMode` — run received bytes through the HDLC decoder instead of raw echo

These are toggled live by console commands in `commands.cpp` (`HANDLE*` functions registered as a `Command` table — `handleHDLC`, `handleLoopback`, `handleEcho`, `handleSend`, `handleBaud`, `handleDump`, `handleStat`, `handleInfo`, `handlePacketGen`, etc.). `HDLCStats sentStats` / `receivedStats` track frame counts.

Shared ring buffers (`src/RingBuffer.{h,cpp}`): `receiveBuffer` and `sendBuffer`. They are used by the raw (non-HDLC) echo and loopback modes; HDLC frames do not go through them. Keep them interrupt safe — do not replace with `std::` containers.

### HDLC frame engine (`src/hdlc.{h,cpp}`)

`class HDLC` is the frame codec. Handles **both**:
- **Bit-Oriented Protocols (BOP):** classic HDLC/SDLC/ADCCP with 0x7E flag, bit stuffing, CRC-16 — fed one bit at a time via `ReceiveBit()` from the PIO bit stream.
- **Character-Oriented Protocols (CCP):** BiSync / DDCMP — fed one byte at a time via `ReceiveByte()`, with configurable SYN character via `EnableCCP()`.

`HDLCReceiveState` is the decoder state machine. `FRAME_STATE` is the completion status of the current frame (`FRAME_EMPTY`, `FRAME_RECEIVING`, `FRAME_OK`, `FRAME_OVERFLOW`, `FRAME_ERROR`). Control-field decoding understands I/S/U frame types; U-frame types are decoded via the `hdlc_u_frame_mappings[]` table (SNRM, SARM, SABM, DISC, UA, DM, RR, RNR, REJ, SREJ, UI, UIH, XID, TEST, etc.). When adding new U-frame support, update that table and the `hdlc_u_frame_type_t` enum together.

The CRC-16 table is `hdlc_fcstab[256]` inside the class — same table as RFC 1662 PPP. `HDLC_CRCINIT = 0xFFFF`, `HDLC_CRCGOOD = 0xF0B8`. Byte-stuffing constants (`HDLC_FLAG 0x7E`, `HDLC_ESCAPE 0x7D`, `HDLC_ESCAPE_MASK 0x20`) are also RFC 1662.

`HDLC_MAX_FRAME_LENGTH = 128` — frames larger than this overflow. If you're extending to larger frames, bump this constant *and* audit `msg[]` sizing and `msg_ptr` width.

### PIO-level sync I/O (`src/sdlc.pio`, `src/sync.cpp`)

The RP2040's PIO does the bit-level work — there is no UART for the RS-422 side. One state machine per direction on `pio0`:
- `sm_rx = 0`
- `sm_tx = 1`

Pin assignments are hard-coded in `src/sync.cpp`:

| Signal | GPIO | Notes |
|---|---|---|
| TX data | GP14 | to RS-422 driver DI |
| TX clock | GP15 | side-set from PIO |
| RX data | GP16 | from RS-422 receiver RO |
| RX clock | GP17 | |

Note: this is currently a **single-channel** PIO implementation despite the 3-channel hardware target (see Hardware section). Expanding to all 3 channels means allocating more state machines across `pio0`/`pio1`.

The PIO RX path has NO per-byte interrupt (changed 2026-11-09: at 983000 bit/s the interrupt could not keep up and the PIO receive FIFO overflowed). A DMA channel copies every received byte from the RX FIFO (joined, 8 deep) into `rxDmaRing` (8 KB, ring-wrapped, transfer count read back to know how many bytes arrived; a second DMA channel re-arms the count). The `RxDecode` task feeds the bytes to `processPioData()` (HDLC mode) or `receiveBuffer` (raw modes). The transmit side uses the PIO "TX FIFO has room" interrupt (PIO0 IRQ1) to wake the sending task; that handler only calls `vTaskNotifyGiveIndexedFromISR`.

### Host control frames (`src/control_frame.{cpp,h}`, `src/console_out.{cpp,h}`)

Decided 2026-11-08: a host program never uses the ESC ESC ESC text console; it sends **control frames** on the USB port instead. A control frame is an ordinary byte-stuffed frame whose address byte is 0xFE AND whose two check bytes are the CRC-16/X.25 register stored un-inverted (CRC over the whole frame = 0x0000; a data frame with a good FCS gives 0xF0B8, so the two never collide). It is handled in `processSerialByteStuffed()`, never put on the line, and does not count as a forwarded frame (so the "console locks after the first forwarded frame" rule never triggers from it). COMMAND frames run the same `processCommand()` as the console; their `printf` output comes back as REPLY frames. The PC tool is `hdlc-loopback` / `hdlc-gateway-tool` in the HdlcGatewayTest repo; `hdlc-loopback --port COMn --command STAT` is the quickest check. The text console stays for humans. **Binary frames to the host must go through `console_out_usb_write()`, never `printf`/`stdio_put_string`** — printf may be redirected into a REPLY capture at any moment.

### Console (`src/console.cpp`, `src/commands.cpp`)

`tskConsole` drains `receiveBuffer` and dispatches to command handlers. Uses blocking `getchar()` via the Pico SDK stdio — this only works once USB CDC has enumerated *and* the host has opened the port. The LED on GP25 is toggled as a liveness indicator (`toggleLED()`).

### Source-file inventory

- `main.cpp` — boot and scheduler start
- `shared.{cpp,h}` — shared globals, `InitializeSharedObjects()`, system state enum, stats
- `sync.{cpp,h}` — PIO setup, TX/RX/decode tasks, DMA receive ring, `hdlcQueue`, byte-stuffed serial path, receive-path measurements for `STAT`
- `hdlc.{cpp,h}` — `HDLC` class (BOP + CCP frame codec, control-field decode)
- `console.{cpp,h}` — console task, receive-buffer dispatch, LED
- `commands.{cpp,h}` — command table and handlers
- `functions.{cpp,h}` — helpers (heap info, FreeRTOS version print, task info, panic helpers)
- `RingBuffer.{cpp,h}` — ISR-safe ring buffer used for TX and RX
- `control_frame.{cpp,h}` — host control frames on the USB port (address 0xFE, un-inverted CRC) and the RFC 1662 byte stuffing; must stay byte-for-byte compatible with the HDLCGW ESP32 firmware and `ControlFrame.cs` in the HdlcGatewayTest repo
- `console_out.{cpp,h}` — handles control frames (HELLO, COMMAND, LOG_LEVEL, STAT_PUSH, PING), captures a command's `printf` output into REPLY frames by swapping the stdio USB driver for a capture driver while the command runs, sends EVENT/STAT frames, and provides `console_out_usb_write()` for binary frames to the host
- `sdlc.pio` — PIO assembly for synchronous TX (clocked bit output); generates `build/sdlc.pio.h`
- `src/backup/sync.cpp` — **legacy, not compiled.** It is not in the `SOURCES` list. Don't edit this file as a way to change behavior; it's a reference copy only.

## Hardware context

The PCB uses **3× MAX3490** (3.3V full-duplex RS-422 transceiver, always-on driver and receiver, no DE/RE) — one chip per channel for 3 TX pairs + 3 RX pairs. This is a **point-to-point full-duplex** design, not a shared RS-485 bus, which is why DE/RE are intentionally absent — do not propose adding driver-enable control logic. Hardware rationale is documented in `docs/RS422_3v3_Transceivers.md` and `docs/MAX3483-MAX3491.pdf`.

The firmware GPIO layout in `sync.cpp` currently services one channel (GP14–GP17). Scaling to all 3 hardware channels is a planned expansion, not done yet.

## FreeRTOS configuration (`include/FreeRTOSConfig.h`)

- `configCPU_CLOCK_HZ = 125_000_000` — 125 MHz, SDK default
- `configTICK_RATE_HZ = 1000` — 1 ms tick
- `configMAX_PRIORITIES = 5` — tasks use `tskIDLE_PRIORITY + 2` as the sync/console baseline
- `configUSE_MUTEXES = 0`, `configUSE_RECURSIVE_MUTEXES = 0`, `configUSE_COUNTING_SEMAPHORES = 0` — **mutexes/semaphores are off.** Coordination is via queues, task notifications, and the ring buffers. Don't reach for `xSemaphoreCreateMutex()` without also enabling it here, and prefer the existing mechanisms.
- `configUSE_TASK_NOTIFICATIONS = 1`, `configTASK_NOTIFICATION_ARRAY_ENTRIES = 3`
- Pico-SDK ISR integration via `vPortSVCHandler`/`xPortPendSVHandler`/`xPortSysTickHandler` aliased to `isr_svcall`/`isr_pendsv`/`isr_systick`

Recent work switched raw `malloc`/`free` in the app code to `pvPortMalloc`/`vPortFree` — keep allocations going through FreeRTOS heap_4. Do not mix libc `malloc` with task-context allocations.

## Testing

There are **no unit tests**. This is on-target firmware; validation is end-to-end on real hardware via the console commands (`loopback`, `echo`, `hdlc`, `packetgen`, `stat`, `inspect`, `decode`) and a scope on GP14–GP17 / the RS-422 pairs. Do not add standalone host-side "test programs" in this repo — if host-side verification is needed, say so and ask before adding any new executable target to CMake.

## Project-specific conventions

- **No `std::` containers** in hot paths or anywhere the RTOS ISRs touch. Use `RingBuffer` for ISR↔task data transfer.
- **Don't add new CMake executables** without being asked — this is a single-binary project.
- **`src/backup/`** is frozen reference code. Do not "refactor" or "sync" it with the live files.
- **Windows-only tooling.** Don't port build scripts to bash/Linux, don't introduce WSL assumptions.
- When you need the user to run an interactive command themselves (e.g., `picotool`, `openocd`, a flash operation that requires physical BOOTSEL), prefix the suggestion with `!` so they can run it inline in the Claude Code session.
