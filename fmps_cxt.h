#ifndef FMPS_CXT_H
#define FMPS_CXT_H

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>

// Pages
#include "pages/main_page.h"
#include "pages/operate_page.h"
#include "pages/read_hitag2_page.h"
#include "pages/read_hf14a_page.h"
#include "pages/status_page.h"
#include "pages/relay_page.h"
#include "pages/relay_source_page.h"
#include "pages/relay_sink_page.h"

typedef enum {
    // The main page of the app, showing the connection status and some basic info
    MainPageViewId,
    // The operate page, showing operation submenu
    OperatePageViewId,
    // Read Hitag2 operation page
    ReadHitag2PageViewId,
    // HF 14a Reader operation page
    ReadHf14aPageViewId,
    // Paginated hardware-status dashboard (Core/Power/Link)
    StatusPageViewId,
    // Relay role selector (Source/Sink)
    RelayPageViewId,
    // Relay Source: read a card and transmit its identity over 433 MHz
    RelaySourcePageViewId,
    // Relay Sink: receive an identity and emulate it to a reader
    RelaySinkPageViewId,
} Proxmark5ViewId;

typedef enum {
    Proxmark5CustomEventOpenOperatePage,
    Proxmark5CustomEventOpenReadHitag2Page,
    Proxmark5CustomEventOpenReadHf14aPage,
    Proxmark5CustomEventOpenStatusPage,
    Proxmark5CustomEventOpenRelayPage,
    Proxmark5CustomEventOpenRelaySourcePage,
    Proxmark5CustomEventOpenRelaySinkPage,
} Proxmark5CustomEvent;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;

    // Pages
    MainPage* main_page;
    OperatePage* operate_page;
    ReadHitag2Page* read_hitag2_page;
    ReadHf14aPage* read_hf14a_page;
    StatusPage* status_page;
    RelayPage* relay_page;
    RelaySourcePage* relay_source_page;
    RelaySinkPage* relay_sink_page;
    // Serializes exclusive use of the single CC1101 between the Source and Sink
    // relay pages so they never init/deinit the radio concurrently.
    FuriMutex* relay_radio_mutex;
} Proxmark5App;

#endif // FMPS_CXT_H
