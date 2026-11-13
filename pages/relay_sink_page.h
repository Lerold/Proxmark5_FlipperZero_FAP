#ifndef RELAY_SINK_PAGE_H
#define RELAY_SINK_PAGE_H

#include <furi.h>
#include <gui/view.h>

// Relay Sink: receives a card identity over the 433 MHz link and drives the
// local PM5 to emulate that ISO14443-A card to a reader.

typedef struct RelaySinkPage RelaySinkPage;

typedef void (*RelaySinkPageBackCallback)(void* context);

// radio_mutex is shared with the Source page; it serializes exclusive use of
// the single CC1101 so the two relay roles never init/deinit the radio at once.
RelaySinkPage* relay_sink_page_create(
    RelaySinkPageBackCallback back_callback,
    void* back_callback_context,
    FuriMutex* radio_mutex);
void relay_sink_page_free(RelaySinkPage* page);

View* relay_sink_page_get_view(RelaySinkPage* page);

void relay_sink_page_start(RelaySinkPage* page);
void relay_sink_page_stop(RelaySinkPage* page);

#endif // RELAY_SINK_PAGE_H
