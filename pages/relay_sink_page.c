#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <string.h>

#include "relay_sink_page.h"
#include "proxmark5_frame.h"
#include "pm3_cmd.h"
#include "relay_proto.h"
#include "subghz_link.h"

// Exact on-wire payload for CMD_HF_ISO14443A_SIMULATE, mirroring the local
// 'struct p' in the firmware's armsrc/appmain.c handler (69 bytes, PACKED).
// Neither pm3_cmd.h declares this struct, so the FAP keeps its own copy in
// lockstep with the firmware. For a plain UID/ATQA/SAK relay all the ulauth /
// rats tail fields stay zero.
typedef struct {
    uint8_t tagtype;
    uint16_t flags;
    uint8_t uid[10];
    uint8_t exitAfter;
    uint8_t rats[20];
    uint8_t ulauth_1a1_len;
    uint8_t ulauth_1a2_len;
    uint8_t ulauth_1a1[16];
    uint8_t ulauth_1a2[16];
    bool ulauth_1a2_mirror;
} __attribute__((packed)) pm3_14a_sim_payload_t;

typedef enum {
    RelaySinkStateStarting,
    RelaySinkStateListening,
    RelaySinkStateEmulating,
    RelaySinkStateLinkError,
} RelaySinkState;

typedef struct {
    RelaySinkState state;
    uint8_t uid[10];
    uint8_t uidlen;
    uint8_t atqa[2];
    uint8_t sak;
    uint32_t rx_count;
} RelaySinkModel;

#define RELAY_SINK_RXBUF_SIZE 128
#define RELAY_SINK_QUEUE_DEPTH 8

struct RelaySinkPage {
    View* view;
    SubGhzLink* link;
    FuriMutex* radio_mutex; // shared with the Source page (not owned here)
    FuriMessageQueue* rx_queue; // RelayUidFrame, filled by the RX callback

    // Reassembly buffer, touched only by the Sub-GHz worker thread (single
    // writer), so no extra locking is needed.
    uint8_t rxbuf[RELAY_SINK_RXBUF_SIZE];
    size_t rxlen;

    FuriThread* worker_thread;
    volatile bool worker_thread_running;
    volatile bool worker_thread_cancel_requested;

    RelaySinkPageBackCallback back_callback;
    void* back_callback_context;
};

// --- frame reassembly (Sub-GHz worker thread) -------------------------------

// Append newly-received bytes and extract any complete, valid relay frames,
// posting each to the queue. Runs on the Sub-GHz worker thread.
static void relay_sink_rx_callback(const uint8_t* data, size_t len, void* context) {
    RelaySinkPage* page = context;
    if(!page || !data || len == 0) {
        return;
    }

    // If the buffer cannot take the new bytes, drop stale leading data.
    if(page->rxlen + len > sizeof(page->rxbuf)) {
        size_t overflow = (page->rxlen + len) - sizeof(page->rxbuf);
        if(overflow >= page->rxlen) {
            page->rxlen = 0;
        } else {
            memmove(page->rxbuf, page->rxbuf + overflow, page->rxlen - overflow);
            page->rxlen -= overflow;
        }
    }
    if(len > sizeof(page->rxbuf)) {
        // A single oversized chunk cannot be a frame; keep only the tail.
        data += (len - sizeof(page->rxbuf));
        len = sizeof(page->rxbuf);
        page->rxlen = 0;
    }
    memcpy(page->rxbuf + page->rxlen, data, len);
    page->rxlen += len;

    // Scan for framed payloads.
    size_t i = 0;
    while(page->rxlen - i >= 2) {
        if(page->rxbuf[i] != RELAY_PROTO_MAGIC0 || page->rxbuf[i + 1] != RELAY_PROTO_MAGIC1) {
            i++;
            continue;
        }
        if(page->rxlen - i < RELAY_UID_FRAME_SIZE) {
            break; // magic found but frame not fully arrived yet
        }
        RelayUidFrame frame;
        if(relay_uid_frame_parse(page->rxbuf + i, RELAY_UID_FRAME_SIZE, &frame)) {
            furi_message_queue_put(page->rx_queue, &frame, 0);
            i += RELAY_UID_FRAME_SIZE;
        } else {
            i++; // bad CRC behind a matching magic - resync one byte on
        }
    }

    // Compact: discard everything we consumed or skipped.
    if(i > 0) {
        if(i >= page->rxlen) {
            page->rxlen = 0;
        } else {
            memmove(page->rxbuf, page->rxbuf + i, page->rxlen - i);
            page->rxlen -= i;
        }
    }
}

// --- PM5 emulation (sink worker thread only) --------------------------------

static void relay_sink_stop_emulation(void) {
    // Any inbound command breaks the firmware's simulate loop (data_available).
    clearCommandBuffer();
    SendCommandNG(CMD_BREAK_LOOP, NULL, 0);
}

static void relay_sink_arm_emulation(const RelayUidFrame* f) {
    // Break any emulation already running, then start the new identity.
    relay_sink_stop_emulation();
    furi_delay_ms(20);

    pm3_14a_sim_payload_t p;
    memset(&p, 0, sizeof(p));
    p.tagtype = 1; // MIFARE Classic 1k defaults (ATQA 0004, SAK 08)
    p.exitAfter = 0; // emulate until the next command
    uint8_t uidlen = f->uidlen > sizeof(p.uid) ? sizeof(p.uid) : f->uidlen;
    FLAG_SET_UID_IN_DATA(p.flags, uidlen); // picks 4B/7B/10B mask from length
    memcpy(p.uid, f->uid, uidlen);

    clearCommandBuffer();
    SendCommandNG(CMD_HF_ISO14443A_SIMULATE, (uint8_t*)&p, sizeof(p));
}

// --- rendering --------------------------------------------------------------

static void format_uid_hex(const uint8_t* uid, uint8_t uidlen, char* out, size_t out_size) {
    if(uidlen > 10) {
        uidlen = 10;
    }
    out[0] = '\0';
    for(uint8_t i = 0; i < uidlen && (size_t)(i * 2 + 2) < out_size; i++) {
        snprintf(&out[i * 2], out_size - (i * 2), "%02X", uid[i]);
    }
}

static void relay_sink_draw_callback(Canvas* canvas, void* context) {
    RelaySinkModel* model = context;
    if(!model) {
        return;
    }

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 12, "Relay Sink");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 4, 24, "433.92 MHz  RX");

    char line[40];
    switch(model->state) {
    case RelaySinkStateStarting:
        canvas_draw_str(canvas, 4, 38, "Starting radio...");
        break;
    case RelaySinkStateListening:
        canvas_draw_str(canvas, 4, 38, "Listening...");
        snprintf(line, sizeof(line), "Frames: %lu", (unsigned long)model->rx_count);
        canvas_draw_str(canvas, 4, 50, line);
        break;
    case RelaySinkStateEmulating: {
        char uid_hex[2 * sizeof(model->uid) + 1];
        format_uid_hex(model->uid, model->uidlen, uid_hex, sizeof(uid_hex));
        snprintf(line, sizeof(line), "Emulating: %s", uid_hex);
        canvas_draw_str(canvas, 4, 38, line);
        snprintf(line, sizeof(line), "ATQA:%02X%02X SAK:%02X F:%lu",
                 model->atqa[1], model->atqa[0], model->sak, (unsigned long)model->rx_count);
        canvas_draw_str(canvas, 4, 50, line);
        break;
    }
    case RelaySinkStateLinkError:
        canvas_draw_str(canvas, 4, 38, "Radio start failed");
        break;
    default:
        break;
    }

    elements_button_left(canvas, "Back");
}

static bool relay_sink_input_callback(InputEvent* event, void* context) {
    RelaySinkPage* page = context;
    if(!page || !event) {
        return false;
    }

    if(event->type == InputTypeShort && event->key == InputKeyLeft) {
        relay_sink_page_stop(page);
        if(page->back_callback) {
            page->back_callback(page->back_callback_context);
        }
        return true;
    }

    return false;
}

// --- worker -----------------------------------------------------------------

static void relay_sink_set_state(RelaySinkPage* page, RelaySinkState state) {
    RelaySinkModel* model = view_get_model(page->view);
    model->state = state;
    view_commit_model(page->view, true);
}

// Acquire exclusive radio ownership, polling so a Back press is still honoured
// while the sibling relay page tears its radio session down. Returns false if
// cancelled before the radio became free. A NULL mutex means no coordination.
static bool relay_sink_acquire_radio(FuriMutex* mutex, volatile bool* cancel) {
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

static bool uid_equal(const uint8_t* a, uint8_t alen, const uint8_t* b, uint8_t blen) {
    if(alen != blen) {
        return false;
    }
    if(alen > 10) {
        alen = 10;
    }
    return memcmp(a, b, alen) == 0;
}

static int32_t relay_sink_worker(void* context) {
    RelaySinkPage* page = context;
    if(!page) {
        return 0;
    }

    relay_sink_set_state(page, RelaySinkStateStarting);

    if(!relay_sink_acquire_radio(page->radio_mutex, &page->worker_thread_cancel_requested)) {
        page->worker_thread_running = false;
        return 0; // cancelled before the radio was free
    }

    page->rxlen = 0;
    if(!subghz_link_start(
           page->link, SUBGHZ_LINK_DEFAULT_FREQUENCY, relay_sink_rx_callback, page)) {
        relay_sink_set_state(page, RelaySinkStateLinkError);
        if(page->radio_mutex) {
            furi_mutex_release(page->radio_mutex);
        }
        page->worker_thread_running = false;
        return 0;
    }

    relay_sink_set_state(page, RelaySinkStateListening);

    uint8_t cur_uid[10];
    uint8_t cur_uidlen = 0;
    bool emulating = false;

    while(!page->worker_thread_cancel_requested) {
        RelayUidFrame f;
        if(furi_message_queue_get(page->rx_queue, &f, 100) != FuriStatusOk) {
            continue; // timeout - re-check the cancel flag
        }

        bool is_new = !emulating || !uid_equal(cur_uid, cur_uidlen, f.uid, f.uidlen);
        uint8_t n = f.uidlen > sizeof(cur_uid) ? sizeof(cur_uid) : f.uidlen;

        if(is_new) {
            // Do the blocking break/delay/re-send with NO view-model lock held:
            // the GUI draw thread takes that same lock, so holding it across
            // furi_delay_ms + SPI sends would stall rendering and input. The
            // read_hf14a template likewise does all device I/O outside the lock.
            relay_sink_arm_emulation(&f);
            memcpy(cur_uid, f.uid, n);
            cur_uidlen = n;
            emulating = true;
        }

        // Keep the locked region to quick field copies only.
        RelaySinkModel* model = view_get_model(page->view);
        model->rx_count++;
        if(is_new) {
            memcpy(model->uid, f.uid, n);
            model->uidlen = n;
            model->atqa[0] = f.atqa[0];
            model->atqa[1] = f.atqa[1];
            model->sak = f.sak;
            model->state = RelaySinkStateEmulating;
        }
        view_commit_model(page->view, true);
    }

    if(emulating) {
        relay_sink_stop_emulation();
    }
    subghz_link_stop(page->link);
    if(page->radio_mutex) {
        furi_mutex_release(page->radio_mutex);
    }
    page->worker_thread_running = false;
    return 0;
}

// --- lifecycle --------------------------------------------------------------

static void relay_sink_cleanup_worker(RelaySinkPage* page) {
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

RelaySinkPage* relay_sink_page_create(
    RelaySinkPageBackCallback back_callback,
    void* back_callback_context,
    FuriMutex* radio_mutex) {
    RelaySinkPage* page = calloc(1, sizeof(RelaySinkPage));
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
    page->rx_queue = furi_message_queue_alloc(RELAY_SINK_QUEUE_DEPTH, sizeof(RelayUidFrame));
    if(!page->link || !page->rx_queue) {
        if(page->rx_queue) furi_message_queue_free(page->rx_queue);
        if(page->link) subghz_link_free(page->link);
        view_free(page->view);
        free(page);
        return NULL;
    }

    page->back_callback = back_callback;
    page->back_callback_context = back_callback_context;

    view_set_context(page->view, page);
    view_allocate_model(page->view, ViewModelTypeLocking, sizeof(RelaySinkModel));
    view_set_draw_callback(page->view, relay_sink_draw_callback);
    view_set_input_callback(page->view, relay_sink_input_callback);

    RelaySinkModel* model = view_get_model(page->view);
    memset(model, 0, sizeof(*model));
    model->state = RelaySinkStateStarting;
    view_commit_model(page->view, false);

    return page;
}

void relay_sink_page_free(RelaySinkPage* page) {
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
    if(page->rx_queue) {
        furi_message_queue_free(page->rx_queue);
        page->rx_queue = NULL;
    }

    if(page->view) {
        view_set_context(page->view, NULL);
        view_free_model(page->view);
        view_free(page->view);
        page->view = NULL;
    }

    free(page);
}

View* relay_sink_page_get_view(RelaySinkPage* page) {
    return page ? page->view : NULL;
}

void relay_sink_page_start(RelaySinkPage* page) {
    if(!page || !page->view) {
        return;
    }

    relay_sink_cleanup_worker(page);
    if(page->worker_thread) {
        return;
    }

    furi_message_queue_reset(page->rx_queue);

    RelaySinkModel* model = view_get_model(page->view);
    memset(model, 0, sizeof(*model));
    model->state = RelaySinkStateStarting;
    view_commit_model(page->view, true);

    page->worker_thread_cancel_requested = false;
    page->worker_thread = furi_thread_alloc_ex("RelaySinkTask", 8192, relay_sink_worker, page);
    if(!page->worker_thread) {
        relay_sink_set_state(page, RelaySinkStateLinkError);
        return;
    }

    page->worker_thread_running = true;
    furi_thread_start(page->worker_thread);
}

void relay_sink_page_stop(RelaySinkPage* page) {
    if(!page || !page->worker_thread) {
        return;
    }
    page->worker_thread_cancel_requested = true;
}
