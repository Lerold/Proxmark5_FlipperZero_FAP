#include <furi.h>
#include "relay_page.h"

typedef enum {
    RelaySubmenuIndexSource = 0,
    RelaySubmenuIndexSink,
} RelaySubmenuIndex;

static void relay_page_submenu_callback(void* context, uint32_t index) {
    RelayPage* relay_page = context;
    if(!relay_page) {
        return;
    }

    switch(index) {
    case RelaySubmenuIndexSource:
        if(relay_page->open_source_callback) {
            relay_page->open_source_callback(relay_page->open_source_callback_context);
        }
        break;
    case RelaySubmenuIndexSink:
        if(relay_page->open_sink_callback) {
            relay_page->open_sink_callback(relay_page->open_sink_callback_context);
        }
        break;
    default:
        break;
    }
}

RelayPage* relay_page_create(
    RelayPageOpenSourceCallback open_source_callback,
    void* open_source_callback_context,
    RelayPageOpenSinkCallback open_sink_callback,
    void* open_sink_callback_context) {
    RelayPage* relay_page = calloc(1, sizeof(RelayPage));
    if(!relay_page) {
        return NULL;
    }

    relay_page->open_source_callback = open_source_callback;
    relay_page->open_source_callback_context = open_source_callback_context;
    relay_page->open_sink_callback = open_sink_callback;
    relay_page->open_sink_callback_context = open_sink_callback_context;

    relay_page->submenu = submenu_alloc();
    submenu_set_header(relay_page->submenu, "Relay Role");
    submenu_add_item(
        relay_page->submenu,
        "Source (read & TX)",
        RelaySubmenuIndexSource,
        relay_page_submenu_callback,
        relay_page);
    submenu_add_item(
        relay_page->submenu,
        "Sink (RX & emulate)",
        RelaySubmenuIndexSink,
        relay_page_submenu_callback,
        relay_page);

    return relay_page;
}

void relay_page_free(RelayPage* relay_page) {
    if(!relay_page) {
        return;
    }

    if(relay_page->submenu) {
        submenu_free(relay_page->submenu);
        relay_page->submenu = NULL;
    }

    free(relay_page);
}
