#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <string.h>

#include "relay_source_page.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"
#include "relay_proto.h"
#include "subghz_link.h"

typedef enum {
    RelaySourceStateStarting,
    RelaySourceStateScanning,
    RelaySourceStateSent,
    RelaySourceStateNoCard,
    RelaySourceStateLinkError,
    RelaySourceStateDeviceError,
} RelaySourceState;

typedef struct {
    RelaySourceState state;
    iso14a_card_select_t card; // last card seen
    uint32_t tx_count; // frames transmitted
    uint32_t unique_count; // distinct UIDs seen
} RelaySourceModel;

struct RelaySourcePage {
    View* view;
    SubGhzLink* link;
    FuriMutex* radio_mutex; // shared with the Sink page (not owned here)

    FuriThread* worker_thread;
    volatile bool worker_thread_running;
    volatile bool worker_thread_cancel_requested;

    RelaySourcePageBackCallback back_callback;
    void* back_callback_context;
};

// --- 14a read (CONNECT-only select), same exchange as read_hf14a_page -------

static int relay_source_wait_response(
    uint16_t cmd,
    PacketResponseNG* resp,
    uint32_t timeout_ms,
    volatile bool* cancel) {
    uint32_t start = furi_get_tick();
    while((furi_get_tick() - start) < timeout_ms) {
        if(cancel && *cancel) {
            return PM3_EOPABORTED;
        }
        if(proxmark5_frame_take_by_cmd(cmd, resp)) {
            return PM3_SUCCESS;
        }
        furi_delay_ms(10);
    }
    if(cancel && *cancel) {
        return PM3_EOPABORTED;
    }
    return proxmark5_frame_take_by_cmd(cmd, resp) ? PM3_SUCCESS : PM3_ETIMEOUT;
}

static int relay_source_read_card(
    volatile bool* cancel,
    iso14a_card_select_t* out_card,
    bool* out_found) {
    if(out_found) {
        *out_found = false;
    }
    if(out_card) {
        memset(out_card, 0, sizeof(*out_card));
    }

    iso14a_raw_cmd_t cmd = {
        .flags = ISO14A_CONNECT | ISO14A_CLEARTRACE,
        .timeout = 0,
        .wait_us = 0,
        .len = 0,
        .lenbits = 0,
    };

    PacketResponseNG* resp = malloc(sizeof(PacketResponseNG));
    if(!resp) {
        return PM3_EMALLOC;
    }

    clearCommandBuffer();
    if(cancel && *cancel) {
        free(resp);
        return PM3_EOPABORTED;
    }

    SendCommandNG(CMD_HF_ISO14443A_READER, (uint8_t*)&cmd, sizeof(cmd));

    int rc = relay_source_wait_response(CMD_HF_ISO14443A_READER, resp, 2000, cancel);
    if(rc != PM3_SUCCESS) {
        free(resp);
        return rc;
    }

    if(resp->status != PM3_SUCCESS ||
       resp->length < sizeof(iso14a_raw_resp_t) + sizeof(iso14a_card_select_t)) {
        free(resp);
        return PM3_ESOFT;
    }

    const iso14a_raw_resp_t* hdr = (const iso14a_raw_resp_t*)resp->data.asBytes;
    if(hdr->sel != 0) {
        if(out_card) {
            memcpy(out_card, resp->data.asBytes + sizeof(iso14a_raw_resp_t), sizeof(*out_card));
        }
        if(out_found) {
            *out_found = true;
        }
    }

    free(resp);
    return PM3_SUCCESS;
}

// --- rendering --------------------------------------------------------------

static void format_uid_hex(const iso14a_card_select_t* card, char* out, size_t out_size) {
    uint8_t uidlen = card->uidlen > sizeof(card->uid) ? sizeof(card->uid) : card->uidlen;
    out[0] = '\0';
    for(uint8_t i = 0; i < uidlen && (size_t)(i * 2 + 2) < out_size; i++) {
        snprintf(&out[i * 2], out_size - (i * 2), "%02X", card->uid[i]);
    }
}

static void relay_source_draw_callback(Canvas* canvas, void* context) {
    RelaySourceModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 12, "Relay Source");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 4, 24, "433.92 MHz  TX");

    char line[40];
    switch(model->state) {
    case RelaySourceStateStarting:
        canvas_draw_str(canvas, 4, 38, "Starting radio...");
        break;
    case RelaySourceStateScanning:
        canvas_draw_str(canvas, 4, 38, "Scanning for card...");
        break;
    case RelaySourceStateSent: {
        char uid_hex[2 * sizeof(model->card.uid) + 1];
        format_uid_hex(&model->card, uid_hex, sizeof(uid_hex));
        snprintf(line, sizeof(line), "TX UID: %s", uid_hex);
        canvas_draw_str(canvas, 4, 38, line);
        snprintf(line, sizeof(line), "Sent: %lu  Uniq: %lu",
                 (unsigned long)model->tx_count, (unsigned long)model->unique_count);
        canvas_draw_str(canvas, 4, 50, line);
        break;
    }
    case RelaySourceStateNoCard:
        canvas_draw_str(canvas, 4, 38, "No card on reader");
        snprintf(line, sizeof(line), "Sent: %lu", (unsigned long)model->tx_count);
        canvas_draw_str(canvas, 4, 50, line);
        break;
    case RelaySourceStateLinkError:
        canvas_draw_str(canvas, 4, 38, "Radio start failed");
        break;
    case RelaySourceStateDeviceError:
        canvas_draw_str(canvas, 4, 38, "PM5 not responding");
        break;
    default:
        break;
    }

    elements_button_left(canvas, "Back");
}

static bool relay_source_input_callback(InputEvent* event, void* context) {
    RelaySourcePage* page = context;
    if(!page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        relay_source_page_stop(page);
        if(page->back_callback) {
            page->back_callback(page->back_callback_context);
        }
        return true;
    }

    return false;
}

// --- worker -----------------------------------------------------------------

static void relay_source_set_state(RelaySourcePage* page, RelaySourceState state) {
    RelaySourceModel* model = view_get_model(page->view);
    model->state = state;
    view_commit_model(page->view, true);
}

// Acquire exclusive radio ownership, polling so a Back press is still honoured
// while the sibling relay page tears its radio session down. Returns false if
// cancelled before the radio became free. A NULL mutex means no coordination.
static bool relay_source_acquire_radio(FuriMutex* mutex, volatile bool* cancel) {
    if(!mutex) {
        return true;
    }
    while(!(cancel && *cancel)) {
        if(furi_mutex_acquire(mutex, 50) == FuriStatusOk) {
            return true;
        }
    }
    return false;
}

// Sleep in short slices so a Back press is honoured promptly.
static void relay_source_sleep(volatile bool* cancel, uint32_t ms) {
    uint32_t start = furi_get_tick();
    while((furi_get_tick() - start) < ms) {
        if(cancel && *cancel) {
            return;
        }
        furi_delay_ms(20);
    }
}

static bool uid_equal(const iso14a_card_select_t* a, const iso14a_card_select_t* b) {
    if(a->uidlen != b->uidlen) {
        return false;
    }
    uint8_t n = a->uidlen > sizeof(a->uid) ? sizeof(a->uid) : a->uidlen;
    return memcmp(a->uid, b->uid, n) == 0;
}

static int32_t relay_source_worker(void* context) {
    RelaySourcePage* page = context;
    if(!page) {
        return 0;
    }

    relay_source_set_state(page, RelaySourceStateStarting);

    if(!relay_source_acquire_radio(
           page->radio_mutex, &page->worker_thread_cancel_requested)) {
        page->worker_thread_running = false;
        return 0; // cancelled before the radio was free
    }

    if(!subghz_link_start(page->link, SUBGHZ_LINK_DEFAULT_FREQUENCY, NULL, NULL)) {
        relay_source_set_state(page, RelaySourceStateLinkError);
        if(page->radio_mutex) {
            furi_mutex_release(page->radio_mutex);
        }
        page->worker_thread_running = false;
        return 0;
    }

    iso14a_card_select_t last_card;
    memset(&last_card, 0, sizeof(last_card));
    bool have_last = false;

    while(!page->worker_thread_cancel_requested) {
        iso14a_card_select_t card;
        bool found = false;
        int rc = relay_source_read_card(&page->worker_thread_cancel_requested, &card, &found);

        if(page->worker_thread_cancel_requested) {
            break;
        }

        if(rc == PM3_ETIMEOUT) {
            relay_source_set_state(page, RelaySourceStateDeviceError);
            relay_source_sleep(&page->worker_thread_cancel_requested, 500);
            continue;
        }

        if(rc != PM3_SUCCESS || !found) {
            relay_source_set_state(page, RelaySourceStateNoCard);
            relay_source_sleep(&page->worker_thread_cancel_requested, 300);
            continue;
        }

        // Found a card - relay its identity.
        RelayUidFrame frame;
        size_t n = relay_uid_frame_build(&frame, card.uid, card.uidlen, card.atqa, card.sak);
        bool sent = subghz_link_send(page->link, (const uint8_t*)&frame, n);

        bool is_new = !have_last || !uid_equal(&card, &last_card);

        RelaySourceModel* model = view_get_model(page->view);
        model->card = card;
        if(sent) {
            model->tx_count++;
            if(is_new) {
                model->unique_count++;
            }
            model->state = RelaySourceStateSent;
        } else {
            model->state = RelaySourceStateLinkError;
        }
        view_commit_model(page->view, true);

        last_card = card;
        have_last = true;

        relay_source_sleep(&page->worker_thread_cancel_requested, 400);
    }

    subghz_link_stop(page->link);
    if(page->radio_mutex) {
        furi_mutex_release(page->radio_mutex);
    }
    page->worker_thread_running = false;
    return 0;
}

// --- lifecycle --------------------------------------------------------------

static void relay_source_cleanup_worker(RelaySourcePage* page) {
    if(!page || !page->worker_thread) {
        return;
    }
    if(page->worker_thread_running) {
        return;
    }
    furi_thread_join(page->worker_thread);
    furi_thread_free(page->worker_thread);
    page->worker_thread = NULL;
}

RelaySourcePage* relay_source_page_create(
    RelaySourcePageBackCallback back_callback,
    void* back_callback_context,
    FuriMutex* radio_mutex) {
    RelaySourcePage* page = calloc(1, sizeof(RelaySourcePage));
    if(!page) {
        return NULL;
    }
    page->radio_mutex = radio_mutex;

    page->view = view_alloc();
    if(!page->view) {
        free(page);
        return NULL;
    }

    page->link = subghz_link_alloc();
    if(!page->link) {
        view_free(page->view);
        free(page);
        return NULL;
    }

    page->back_callback = back_callback;
    page->back_callback_context = back_callback_context;

    view_set_context(page->view, page);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(RelaySourceModel));
    view_set_draw_callback(page->view, relay_source_draw_callback);
    view_set_input_callback(page->view, relay_source_input_callback);

    RelaySourceModel* model = view_get_model(page->view);
    model->state = RelaySourceStateStarting;
    model->tx_count = 0;
    model->unique_count = 0;
    memset(&model->card, 0, sizeof(model->card));
    view_commit_model(page->view, false);

    return page;
}

void relay_source_page_free(RelaySourcePage* page) {
    if(!page) {
        return;
    }

    page->worker_thread_cancel_requested = true;
    if(page->worker_thread) {
        furi_thread_join(page->worker_thread);
        furi_thread_free(page->worker_thread);
        page->worker_thread = NULL;
        page->worker_thread_running = false;
    }

    if(page->link) {
        subghz_link_free(page->link);
        page->link = NULL;
    }

    if(page->view) {
        view_set_context(page->view, NULL);
        view_free_model(page->view);
        view_free(page->view);
        page->view = NULL;
    }

    free(page);
}

View* relay_source_page_get_view(RelaySourcePage* page) {
    return page ? page->view : NULL;
}

void relay_source_page_start(RelaySourcePage* page) {
    if(!page || !page->view) {
        return;
    }

    relay_source_cleanup_worker(page);
    if(page->worker_thread) {
        return;
    }

    RelaySourceModel* model = view_get_model(page->view);
    model->state = RelaySourceStateStarting;
    model->tx_count = 0;
    model->unique_count = 0;
    memset(&model->card, 0, sizeof(model->card));
    view_commit_model(page->view, true);

    page->worker_thread_cancel_requested = false;
    page->worker_thread =
        furi_thread_alloc_ex("RelaySourceTask", 8192, relay_source_worker, page);
    if(!page->worker_thread) {
        relay_source_set_state(page, RelaySourceStateDeviceError);
        return;
    }

    page->worker_thread_running = true;
    furi_thread_start(page->worker_thread);
}

void relay_source_page_stop(RelaySourcePage* page) {
    if(!page || !page->worker_thread) {
        return;
    }
    page->worker_thread_cancel_requested = true;
}
