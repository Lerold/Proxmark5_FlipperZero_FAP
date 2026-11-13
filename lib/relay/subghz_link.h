#ifndef SUBGHZ_LINK_H
#define SUBGHZ_LINK_H

// A thin half-duplex data link over the Flipper's internal CC1101, used to
// carry relay frames between the two Flippers. Wraps the firmware's
// SubGhzTxRxWorker: a single worker instance both transmits and receives
// (it flips to Tx on write and returns to Rx), so one link object is enough.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 433.92 MHz, the band the demo uses. Valid on the default Flipper regions.
#define SUBGHZ_LINK_DEFAULT_FREQUENCY 433920000UL

// The worker rejects writes larger than this (SUBGHZ_TXRX_WORKER_MAX_TXRX_SIZE).
#define SUBGHZ_LINK_MAX_FRAME 60

typedef struct SubGhzLink SubGhzLink;

// Invoked from the Sub-GHz worker thread whenever received bytes are drained.
// Delivers raw bytes (no framing) - the caller reassembles/validates frames.
typedef void (*SubGhzLinkRxCallback)(const uint8_t* data, size_t len, void* context);

SubGhzLink* subghz_link_alloc(void);
void subghz_link_free(SubGhzLink* link);

// Acquire the radio and start the worker on `frequency` Hz. rx_callback may be
// NULL for a transmit-only role. Returns false on an invalid frequency or if
// the radio/worker could not be started.
bool subghz_link_start(
    SubGhzLink* link,
    uint32_t frequency,
    SubGhzLinkRxCallback rx_callback,
    void* rx_context);

// Stop the worker and release the radio. Safe to call when not running.
void subghz_link_stop(SubGhzLink* link);

bool subghz_link_is_running(SubGhzLink* link);

// Transmit one frame. Returns false if len > SUBGHZ_LINK_MAX_FRAME or the link
// is not running.
bool subghz_link_send(SubGhzLink* link, const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // SUBGHZ_LINK_H
