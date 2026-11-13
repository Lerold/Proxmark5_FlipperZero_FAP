#include "subghz_link.h"

#include <furi.h>
#include <furi_hal_subghz.h>
#include <lib/subghz/subghz_tx_rx_worker.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>

struct SubGhzLink {
    SubGhzTxRxWorker* worker;
    const SubGhzDevice* device;
    SubGhzLinkRxCallback rx_callback;
    void* rx_context;
    bool devices_inited;
};

// Scratch buffer sized to the worker's max payload; the have-read callback
// drains into it in chunks and forwards them to the user callback.
#define SUBGHZ_LINK_RX_CHUNK SUBGHZ_LINK_MAX_FRAME

SubGhzLink* subghz_link_alloc(void) {
    SubGhzLink* link = calloc(1, sizeof(SubGhzLink));
    return link;
}

void subghz_link_free(SubGhzLink* link) {
    if(!link) {
        return;
    }
    subghz_link_stop(link);
    free(link);
}

// Runs on the Sub-GHz worker thread. Drain everything currently buffered and
// hand it to the consumer, which does the frame reassembly.
static void subghz_link_have_read_callback(void* context) {
    SubGhzLink* link = context;
    if(!link || !link->worker) {
        return;
    }

    uint8_t chunk[SUBGHZ_LINK_RX_CHUNK];
    while(subghz_tx_rx_worker_available(link->worker) > 0) {
        size_t n = subghz_tx_rx_worker_read(link->worker, chunk, sizeof(chunk));
        if(n == 0) {
            break;
        }
        if(link->rx_callback) {
            link->rx_callback(chunk, n, link->rx_context);
        }
    }
}

bool subghz_link_start(
    SubGhzLink* link,
    uint32_t frequency,
    SubGhzLinkRxCallback rx_callback,
    void* rx_context) {
    if(!link || link->worker) {
        return false;
    }

    if(!furi_hal_subghz_is_frequency_valid(frequency)) {
        return false;
    }

    link->rx_callback = rx_callback;
    link->rx_context = rx_context;

    subghz_devices_init();
    link->devices_inited = true;

    link->device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    if(!link->device) {
        subghz_devices_deinit();
        link->devices_inited = false;
        return false;
    }

    link->worker = subghz_tx_rx_worker_alloc();
    if(!link->worker) {
        subghz_devices_deinit();
        link->devices_inited = false;
        return false;
    }

    subghz_tx_rx_worker_set_callback_have_read(
        link->worker, subghz_link_have_read_callback, link);

    if(!subghz_tx_rx_worker_start(link->worker, link->device, frequency)) {
        subghz_tx_rx_worker_free(link->worker);
        link->worker = NULL;
        subghz_devices_deinit();
        link->devices_inited = false;
        return false;
    }

    return true;
}

void subghz_link_stop(SubGhzLink* link) {
    if(!link) {
        return;
    }

    if(link->worker) {
        if(subghz_tx_rx_worker_is_running(link->worker)) {
            subghz_tx_rx_worker_stop(link->worker);
        }
        subghz_tx_rx_worker_free(link->worker);
        link->worker = NULL;
    }

    if(link->devices_inited) {
        subghz_devices_deinit();
        link->devices_inited = false;
    }

    link->device = NULL;
    link->rx_callback = NULL;
    link->rx_context = NULL;
}

bool subghz_link_is_running(SubGhzLink* link) {
    return link && link->worker && subghz_tx_rx_worker_is_running(link->worker);
}

bool subghz_link_send(SubGhzLink* link, const uint8_t* data, size_t len) {
    if(!link || !link->worker || !data) {
        return false;
    }
    if(len == 0 || len > SUBGHZ_LINK_MAX_FRAME) {
        return false;
    }
    // The worker API takes a non-const pointer but does not modify the buffer.
    return subghz_tx_rx_worker_write(link->worker, (uint8_t*)data, len);
}
