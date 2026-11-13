#ifndef RELAY_PROTO_H
#define RELAY_PROTO_H

// Wire protocol for the PM5<->Flipper<->(433MHz)<->Flipper<->PM5 relay.
//
// Stage 1 relays only the ISO14443-A card identity (UID / ATQA / SAK) so a
// second Proxmark can present the same card to a reader. The frame is a small,
// self-describing, CRC-protected blob so stray 433 MHz traffic is not mistaken
// for a relay frame. It fits comfortably inside the Sub-GHz worker's 60-byte
// per-write limit.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_PROTO_MAGIC0  0x50 // 'P'
#define RELAY_PROTO_MAGIC1  0x35 // '5'
#define RELAY_PROTO_VERSION 0x01

typedef enum {
    RelayFrameTypeUid = 1,
} RelayFrameType;

// PACKED so the on-wire layout is identical on both Flippers regardless of the
// compiler's alignment choices. Total size = 20 bytes.
typedef struct {
    uint8_t magic0; // RELAY_PROTO_MAGIC0
    uint8_t magic1; // RELAY_PROTO_MAGIC1
    uint8_t version; // RELAY_PROTO_VERSION
    uint8_t type; // RelayFrameType
    uint8_t uidlen; // 4, 7 or 10
    uint8_t uid[10];
    uint8_t atqa[2];
    uint8_t sak;
    uint16_t crc; // CRC-16/CCITT-FALSE over the frame up to (not incl.) crc
} __attribute__((packed)) RelayUidFrame;

#define RELAY_UID_FRAME_SIZE (sizeof(RelayUidFrame))

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xorout).
uint16_t relay_crc16(const uint8_t* data, size_t len);

// Build a UID frame into *f (sets magic/version/type and computes the CRC).
// uidlen is clamped to the uid[] capacity. Returns RELAY_UID_FRAME_SIZE.
size_t relay_uid_frame_build(
    RelayUidFrame* f,
    const uint8_t* uid,
    uint8_t uidlen,
    const uint8_t atqa[2],
    uint8_t sak);

// Validate buf as a UID frame: exact size, magic, version, type and CRC. On
// success copies it into *out (if non-NULL) and returns true.
bool relay_uid_frame_parse(const uint8_t* buf, size_t len, RelayUidFrame* out);

#ifdef __cplusplus
}
#endif

#endif // RELAY_PROTO_H
