/*
 * Memory card saves (phase 8) — see memcard.hh.
 */
#include "memcard.hh"

#include <new>
#include <stdlib.h>
#include <string.h>

#include <psyqo/alloc.h>
#include <psyqo/gpu.hh>

extern psyqo::GPU *g_ps1GPU;

namespace {

// Placement-constructed in PS1McPrepare: no global constructors (nothing runs before prepare()).
alignas(psyqo::MemoryCard) uint8_t s_cardStorage[sizeof(psyqo::MemoryCard)];
alignas(psyqo::MemoryCardFileSystem) uint8_t s_fsStorage[sizeof(psyqo::MemoryCardFileSystem)];
psyqo::MemoryCardFileSystem *s_fs = nullptr;

struct Header {
    char magic[4]; // "RSDK"
    uint16_t version;
    uint16_t reserved;
    uint32_t length;
    uint32_t crc;
};
static_assert(sizeof(Header) == PS1_MC_HEADER_SIZE, "payload header is 16 bytes");

uint32_t crc32(const uint8_t *p, uint32_t n) { // zlib CRC-32 (reflected 0xEDB88320), bitwise
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

bool ready() { return s_fs && g_ps1GPU; }

} // namespace

void PS1McPrepare() {
    if (s_fs)
        return;
    auto *card = new (s_cardStorage) psyqo::MemoryCard();
    card->prepare();
    s_fs = new (s_fsStorage) psyqo::MemoryCardFileSystem(*card);
}

const char *PS1McErrorMessage(PS1McError error) { return psyqo::MemoryCard::errorMessage(error); }

bool PS1McFileName(char out[21], char region, const char *product, const char *name) {
    size_t p = strlen(product), n = strlen(name);
    if (p != 10 || n > 8)
        return false;
    out[0] = 'B';
    out[1] = region;
    memcpy(out + 2, product, p);
    memcpy(out + 12, name, n);
    out[12 + n] = 0;
    return true;
}

PS1McError PS1McState(PS1McPort port) {
    return ready() ? s_fs->getCardStateBlocking(*g_ps1GPU, port) : PS1McError::Unconnected;
}

PS1McError PS1McFreeBlocks(PS1McPort port, uint32_t *outFree) {
    return ready() ? s_fs->getFreeBlockCountBlocking(*g_ps1GPU, port, outFree) : PS1McError::Unconnected;
}

PS1McError PS1McExists(PS1McPort port, const char *file, bool *outExists) {
    return ready() ? s_fs->fileExistsBlocking(*g_ps1GPU, port, file, outExists) : PS1McError::Unconnected;
}

PS1McError PS1McList(PS1McPort port, psyqo::MemoryCardFileSystem::FileEntry *out, uint32_t max, uint32_t *outCount) {
    return ready() ? s_fs->listFilesBlocking(*g_ps1GPU, port, out, max, outCount) : PS1McError::Unconnected;
}

PS1McError PS1McInfo(PS1McPort port, const char *file, psyqo::MemoryCardFileSystem::FileInfo *out) {
    return ready() ? s_fs->readFileInfoBlocking(*g_ps1GPU, port, file, out) : PS1McError::Unconnected;
}

PS1McError PS1McDelete(PS1McPort port, const char *file) {
    return ready() ? s_fs->deleteFileBlocking(*g_ps1GPU, port, file) : PS1McError::Unconnected;
}

PS1McError PS1McSave(PS1McPort port, const char *file, const char *title, const PS1McIcon &icon, const void *data,
                     uint32_t len, uint16_t version) {
    if (!ready())
        return PS1McError::Unconnected;
    bool exists = false;
    PS1McError e = s_fs->fileExistsBlocking(*g_ps1GPU, port, file, &exists);
    if (e != PS1McError::OK)
        return e;
    uint8_t *buf = (uint8_t *)psyqo_malloc(PS1_MC_HEADER_SIZE + len);
    if (!buf)
        return PS1McError::FileTooLarge;
    Header h;
    memcpy(h.magic, "RSDK", 4);
    h.version  = version;
    h.reserved = 0;
    h.length   = len;
    h.crc      = crc32((const uint8_t *)data, len);
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), data, len);
    if (exists)
        e = s_fs->deleteFileBlocking(*g_ps1GPU, port, file);
    if (e == PS1McError::OK)
        e = s_fs->writeFileBlocking(*g_ps1GPU, port, file, title, icon, buf, PS1_MC_HEADER_SIZE + len);
    psyqo_free(buf);
    return e;
}

PS1McError PS1McLoad(PS1McPort port, const char *file, void *buf, uint32_t maxLen, uint32_t *outLen,
                     uint16_t *outVersion) {
    if (outLen)
        *outLen = 0;
    if (!ready())
        return PS1McError::Unconnected;
    // psyqo returns whole blocks of payload, cut at the buffer size: the header always fits, so an
    // oversized save is reported (SerializeOverflow) rather than silently truncated.
    uint32_t cap = PS1_MC_HEADER_SIZE + maxLen;
    uint8_t *tmp = (uint8_t *)psyqo_malloc(cap);
    if (!tmp)
        return PS1McError::SerializeOverflow;
    uint32_t got = 0;
    PS1McError e = s_fs->readFileBlocking(*g_ps1GPU, port, file, tmp, cap, &got);
    if (e == PS1McError::OK) {
        Header h;
        memcpy(&h, tmp, sizeof(h));
        if (got < sizeof(h) || memcmp(h.magic, "RSDK", 4))
            e = PS1McError::BadData;
        else if (h.length > maxLen)
            e = PS1McError::SerializeOverflow;
        else if (sizeof(h) + h.length > got || crc32(tmp + sizeof(h), h.length) != h.crc)
            e = PS1McError::BadData;
        else {
            memcpy(buf, tmp + sizeof(h), h.length);
            if (outLen)
                *outLen = h.length;
            if (outVersion)
                *outVersion = h.version;
        }
    }
    psyqo_free(tmp);
    return e;
}
