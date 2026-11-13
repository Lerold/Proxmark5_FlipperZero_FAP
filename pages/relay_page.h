#ifndef RELAY_PAGE_H
#define RELAY_PAGE_H

#include <gui/modules/submenu.h>

// Role selector for the relay feature: pick Source (read a card and transmit
// its identity) or Sink (receive an identity and emulate it to a reader).

typedef void (*RelayPageOpenSourceCallback)(void* context);
typedef void (*RelayPageOpenSinkCallback)(void* context);

typedef struct {
    Submenu* submenu;

    RelayPageOpenSourceCallback open_source_callback;
    void* open_source_callback_context;
    RelayPageOpenSinkCallback open_sink_callback;
    void* open_sink_callback_context;
} RelayPage;

RelayPage* relay_page_create(
    RelayPageOpenSourceCallback open_source_callback,
    void* open_source_callback_context,
    RelayPageOpenSinkCallback open_sink_callback,
    void* open_sink_callback_context);
void relay_page_free(RelayPage* relay_page);

#endif // RELAY_PAGE_H
