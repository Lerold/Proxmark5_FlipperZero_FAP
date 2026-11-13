#include "relay_proto.h"
#include <string.h>

uint16_t relay_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for(size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for(uint8_t b = 0; b < 8; b++) {
            if(crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

size_t relay_uid_frame_build(
    RelayUidFrame* f,
    const uint8_t* uid,
    uint8_t uidlen,
    const uint8_t atqa[2],
    uint8_t sak) {
    if(!f) {
        return 0;
    }

    memset(f, 0, sizeof(*f));
    f->magic0 = RELAY_PROTO_MAGIC0;
    f->magic1 = RELAY_PROTO_MAGIC1;
    f->version = RELAY_PROTO_VERSION;
    f->type = RelayFrameTypeUid;

    if(uidlen > sizeof(f->uid)) {
        uidlen = sizeof(f->uid);
    }
    f->uidlen = uidlen;
    if(uid && uidlen) {
        memcpy(f->uid, uid, uidlen);
    }
    if(atqa) {
        f->atqa[0] = atqa[0];
        f->atqa[1] = atqa[1];
    }
    f->sak = sak;

    // CRC covers every byte except the trailing crc field itself.
    f->crc = relay_crc16((const uint8_t*)f, sizeof(*f) - sizeof(f->crc));
    return sizeof(*f);
}

bool relay_uid_frame_parse(const uint8_t* buf, size_t len, RelayUidFrame* out) {
    if(!buf || len != RELAY_UID_FRAME_SIZE) {
        return false;
    }

    RelayUidFrame f;
    memcpy(&f, buf, sizeof(f));

    if(f.magic0 != RELAY_PROTO_MAGIC0 || f.magic1 != RELAY_PROTO_MAGIC1) {
        return false;
    }
    if(f.version != RELAY_PROTO_VERSION || f.type != RelayFrameTypeUid) {
        return false;
    }
    if(f.uidlen > sizeof(f.uid)) {
        return false;
    }

    uint16_t want = relay_crc16((const uint8_t*)&f, sizeof(f) - sizeof(f.crc));
    if(want != f.crc) {
        return false;
    }

    if(out) {
        memcpy(out, &f, sizeof(f));
    }
    return true;
}
