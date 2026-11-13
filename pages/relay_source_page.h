#ifndef RELAY_SOURCE_PAGE_H
#define RELAY_SOURCE_PAGE_H

#include <furi.h>
#include <gui/view.h>

// Relay Source: repeatedly reads an ISO14443-A card on the local PM5 and
// transmits its identity (UID/ATQA/SAK) over the 433 MHz link to the Sink.

typedef struct RelaySourcePage RelaySourcePage;

typedef void (*RelaySourcePageBackCallback)(void* context);

// radio_mutex is shared with the Sink page; it serializes exclusive use of the
// single CC1101 so the two relay roles never init/deinit the radio at once.
RelaySourcePage* relay_source_page_create(
    RelaySourcePageBackCallback back_callback,
    void* back_callback_context,
    FuriMutex* radio_mutex);
void relay_source_page_free(RelaySourcePage* page);

View* relay_source_page_get_view(RelaySourcePage* page);

void relay_source_page_start(RelaySourcePage* page);
void relay_source_page_stop(RelaySourcePage* page);

#endif // RELAY_SOURCE_PAGE_H
