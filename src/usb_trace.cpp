/**************************************************************************
** USB_TRACE - what the Pico's USB device stack actually started and     **
**             completed on the wire, per endpoint                       **
**                                                                       **
** WHY: on 2026-11-08 every forwarded frame was handed to the USB stdio  **
** driver and accepted (writes complete in under 330 us), yet about 1 in **
** 100 whole frames never reached the PC, which received byte for byte   **
** exactly the frames it decoded. So bytes vanish between TinyUSB's      **
** FIFO and the cable. The RP2040 device driver has one silent discard   **
** (hw_endpoint_xfer_start resets a transfer that is still active). This **
** file measures, without touching the SDK, by wrapping two cross-file   **
** TinyUSB calls at link time (see --wrap in CMakeLists.txt):            **
**   usbd_edpt_xfer       every transfer START (endpoint, bytes)         **
**   dcd_event_handler    every event from the hardware; the completion  **
**                        events carry the endpoint and the bytes done   **
** Bytes handed == started == completed means the device sent every      **
** byte and the loss is on the host; started > completed means the      **
** device driver discarded them.                                         **
***************************************************************************/
#include <stdint.h>
#include <stdbool.h>

#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include "pico/time.h"

#include "usb_trace.h"

// Index 0..15: IN endpoints 0x80..0x8F; the CDC data IN endpoint is 0x82 (stdio_usb_descriptors.c)
static volatile uint32_t inStarts[16];
static volatile uint32_t inStartBytes[16];
static volatile uint32_t inCompletes[16];
static volatile uint32_t inCompleteBytes[16];
static volatile uint32_t inCompleteFailed[16]; // completions with a result other than success

// Raw bus events from the RP2040 USB block, counted before TinyUSB gates any callback.
// usbd.c sets its "suspended" flag directly in this event path on SUSPEND and clears it on
// RESUME; while it is set tud_ready() is false, tud_cdc_connected() is false, and the SDK's
// USB stdio write returns at once without writing. So these counts, with the time of the last
// suspend, say whether the host-side frame loss lines up with a suspend.
static volatile uint32_t busSuspends = 0;
static volatile uint32_t busResumes = 0;
static volatile uint32_t busResets = 0;
static volatile uint32_t busUnplugs = 0;
static volatile uint32_t busSofCount = 0;
static volatile uint32_t lastSuspendMs = 0;

extern "C" bool __real_usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes);
extern "C" void __real_dcd_event_handler(dcd_event_t const *event, bool in_isr);

// The CDC FIFO stage: bytes the SDK's write asked TinyUSB to take, bytes TinyUSB accepted into
// its 64-byte CDC TX FIFO, and bytes the flush pulled out of that FIFO into transfers. These are
// cross-file calls from stdio_usb.c into cdc_device.c, wrapped at link time like the two above.
static volatile uint32_t cdcWriteCalls = 0;
static volatile uint32_t cdcWriteRequested = 0;
static volatile uint32_t cdcWriteAccepted = 0;
static volatile uint32_t cdcFlushCalls = 0;
static volatile uint32_t cdcFlushBytes = 0;

extern "C" uint32_t __real_tud_cdc_n_write(uint8_t itf, void const *buffer, uint32_t bufsize);
extern "C" uint32_t __real_tud_cdc_n_write_flush(uint8_t itf);

extern "C" uint32_t __wrap_tud_cdc_n_write(uint8_t itf, void const *buffer, uint32_t bufsize)
{
    uint32_t accepted = __real_tud_cdc_n_write(itf, buffer, bufsize);
    cdcWriteCalls++;
    cdcWriteRequested += bufsize;
    cdcWriteAccepted += accepted;
    return accepted;
}

extern "C" uint32_t __wrap_tud_cdc_n_write_flush(uint8_t itf)
{
    uint32_t n = __real_tud_cdc_n_write_flush(itf);
    cdcFlushCalls++;
    cdcFlushBytes += n;
    return n;
}

void usb_trace_cdc_stage(uint32_t *writeCalls, uint32_t *requested, uint32_t *accepted, uint32_t *flushCalls,
                         uint32_t *flushBytes)
{
    *writeCalls = cdcWriteCalls;
    *requested = cdcWriteRequested;
    *accepted = cdcWriteAccepted;
    *flushCalls = cdcFlushCalls;
    *flushBytes = cdcFlushBytes;
}

extern "C" bool __wrap_usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes)
{
    if (ep_addr & 0x80)
    {
        uint8_t i = ep_addr & 0x0F;
        inStarts[i]++;
        inStartBytes[i] += total_bytes;
    }
    return __real_usbd_edpt_xfer(rhport, ep_addr, buffer, total_bytes);
}

extern "C" void __wrap_dcd_event_handler(dcd_event_t const *event, bool in_isr)
{
    switch (event->event_id)
    {
    case DCD_EVENT_XFER_COMPLETE:
        if (event->xfer_complete.ep_addr & 0x80)
        {
            uint8_t i = event->xfer_complete.ep_addr & 0x0F;
            inCompletes[i]++;
            inCompleteBytes[i] += event->xfer_complete.len;
            if (event->xfer_complete.result != XFER_RESULT_SUCCESS)
            {
                inCompleteFailed[i]++;
            }
        }
        break;
    case DCD_EVENT_SUSPEND:
        busSuspends++;
        lastSuspendMs = to_ms_since_boot(get_absolute_time());
        break;
    case DCD_EVENT_RESUME:
        busResumes++;
        break;
    case DCD_EVENT_BUS_RESET:
        busResets++;
        break;
    case DCD_EVENT_UNPLUGGED:
        busUnplugs++;
        break;
    case DCD_EVENT_SOF:
        busSofCount++;
        break;
    default:
        break;
    }
    __real_dcd_event_handler(event, in_isr);
}

void usb_trace_bus_events(uint32_t *suspends, uint32_t *resumes, uint32_t *resets, uint32_t *unplugs,
                          uint32_t *lastSuspendMsOut)
{
    *suspends = busSuspends;
    *resumes = busResumes;
    *resets = busResets;
    *unplugs = busUnplugs;
    *lastSuspendMsOut = lastSuspendMs;
}

void usb_trace_in_endpoint(uint8_t ep_in, uint32_t *starts, uint32_t *startBytes, uint32_t *completes,
                           uint32_t *completeBytes, uint32_t *failed)
{
    uint8_t i = ep_in & 0x0F;
    *starts = inStarts[i];
    *startBytes = inStartBytes[i];
    *completes = inCompletes[i];
    *completeBytes = inCompleteBytes[i];
    *failed = inCompleteFailed[i];
}
