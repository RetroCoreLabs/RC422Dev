/**************************************************************************
** USB_TRACE - per-endpoint count of USB IN transfers started and        **
**             completed, from inside TinyUSB (link-time wraps)          **
***************************************************************************/
#ifndef USB_TRACE_H
#define USB_TRACE_H

#include <stdint.h>

/** The CDC data IN endpoint used by the Pico SDK's USB stdio (stdio_usb_descriptors.c). */
#define USB_TRACE_CDC_EP_IN 0x82

/** Counters for one IN endpoint: transfers started (and their bytes), completions reported
 *  by the hardware (and their bytes), and completions whose result was not success. */
void usb_trace_in_endpoint(uint8_t ep_in, uint32_t *starts, uint32_t *startBytes, uint32_t *completes,
                           uint32_t *completeBytes, uint32_t *failed);

/** Raw USB bus events seen by the device driver: suspends (with ms since boot of the last one),
 *  resumes, bus resets, unplugs. Counted before TinyUSB decides whether to call any callback. */
void usb_trace_bus_events(uint32_t *suspends, uint32_t *resumes, uint32_t *resets, uint32_t *unplugs,
                          uint32_t *lastSuspendMsOut);

/** The CDC FIFO stage: tud_cdc_n_write calls, bytes requested of it, bytes it accepted into the
 *  FIFO, tud_cdc_n_write_flush calls and the bytes those flushes moved into transfers. */
void usb_trace_cdc_stage(uint32_t *writeCalls, uint32_t *requested, uint32_t *accepted, uint32_t *flushCalls,
                         uint32_t *flushBytes);

#endif /* USB_TRACE_H */
