/*
 * PS1 port — CD-ROM file I/O (Phase 6)
 *
 * Synchronous ISO9660 reader on top of psyqo::CDRomDevice::readSectorsBlocking,
 * exposed as the standard <stdio.h> FILE* API so the untouched RSDKv2 Reader.cpp
 * (fopen/fread/fseek/ftell/fclose + ParseVirtualFileSystem) works unchanged.
 *
 * Only root-level files are looked up (sufficient for "Data.bin", the single
 * virtual-filesystem file the engine reads). Nested paths (loose Data/ folder
 * fallback) are deferred.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EASTL/atomic.h>
#include <psyqo/cdrom-device.hh>
#include <psyqo/gpu.hh>
#include <psyqo/hardware/cdrom.hh>
#include <psyqo/hardware/sbus.hh>
#include <psyqo/kernel.hh>
#include <psyqo/msf.hh>
#include "common/hardware/dma.h"
#include "common/syscalls/syscalls.h"

#include "video.hh"

extern psyqo::CDRomDevice *g_ps1CDRom;
volatile uint32_t g_ps1VideoForcedClose = 0; // file reads that had to stop an open FMV stream
extern psyqo::GPU         *g_ps1GPU;

#define ISO_SECTOR 2048
#define DIRECT_MAX   32 /* sector-aligned reads straight into the caller's buffer: up to 64KB per call */

static uint8_t *s_sec;

/* Read-ahead windows (load-time phase). Every blocking request costs a Setloc/ReadN(seek)/Pause
 * cycle, which dominates load times (the engine refills 8 KB at a time and re-opens config files
 * over and over). A buffered read that misses fills a window of WINDOW_SECTORS sectors and later
 * reads are served from RAM: during a load from the load stream (below), which keeps reading into
 * the next window; outside a load with one blocking request. The window runs past the file's end:
 * tools/disc/build_disc.py lays the data files out in load order, so it usually holds the next files
 * the game opens (the CD-XA/STR streams sit after all data; read as data they are just unused
 * sectors). Three LRU slots (the engine re-reads GameConfig between the SFX it loads), heap-allocated
 * as needed, freed after each load (PS1FsReleaseWindows from PS1LoadingEnd) or at fclose outside a
 * load. 16 sectors: with the stream a window no longer costs a request (R11A 6.93 s vs 6.69 s with
 * 32), and the load peak keeps 82 KB more heap. */
#define WINDOW_SECTORS 16
#define WINDOWS        3
struct Window {
    uint8_t *buf;
    volatile uint32_t lba, count, age; // lba/count also written by the load stream's IRQ
    volatile bool ahead;               // filled by the load stream, not yet read
};
static Window s_win[WINDOWS];
static bool readViaWindows(uint32_t lba, uint32_t bytes, uint8_t *dst);
static uint32_t s_winClock    = 0;
static uint32_t s_volumeEnd   = 0; // sectors in the volume (PVD): read-ahead never goes past it
volatile uint32_t g_ps1FsWindowHits  = 0; // buffered reads served from a window (GDB)
volatile uint32_t g_ps1FsWindowFills = 0; // window misses (one CD request each)
volatile uint32_t g_ps1FsFallbackReads = 0; // reads through the 1-sector fallback (no heap for a window)
// When the heap can't hold a window (outside a load the renderer's chain arenas live there too,
// ps1/gpu_chain), reads go through this one-sector buffer: slower, never short.
static uint8_t s_fallbackBuf[2048];
static Window s_fallback = {s_fallbackBuf, 0, 0, 0, false};

/* findEntry reads the volume descriptor and every directory on the path; the disc is read-only, so
 * those sectors are cached (LRU). With the file index (below) only paths outside it walk directories:
 * three entries (volume descriptor, root, one more). */
#define DIR_CACHE 3
static uint8_t s_dirCache[DIR_CACHE][ISO_SECTOR];
static uint32_t s_dirCacheLba[DIR_CACHE];
static uint32_t s_dirCacheAge[DIR_CACHE];
static uint32_t s_dirCacheClock = 0;
volatile uint32_t g_ps1FsDirReads = 0; // directory sectors read from the CD (cache misses, GDB)
volatile uint32_t g_ps1FsDirHits  = 0;

volatile uint32_t g_ps1FsRequests = 0; // blocking CD requests (each = Setloc/ReadN/Pause), GDB
volatile uint32_t g_ps1FsSectors  = 0; // sectors those requests read

volatile uint32_t g_ps1FsReadUs   = 0; // time spent inside them (microseconds)

// GDB profiling point (tools/load_profile.sh): one call per request, after it completed.
extern "C" __attribute__((noinline)) void PS1FsProfileHook(uint32_t lba, uint32_t count, uint32_t us) {
    __asm__ volatile("" ::"r"(lba), "r"(count), "r"(us));
}

static void lsClose();
bool PS1LoadingActive(); // loading.cpp

static void countRequest(uint32_t lba, uint32_t count, uint32_t us) {
    g_ps1FsRequests = g_ps1FsRequests + 1;
    g_ps1FsSectors  = g_ps1FsSectors + count;
    g_ps1FsReadUs   = g_ps1FsReadUs + us;
    PS1FsProfileHook(lba, count, us);
}

// Every file-system CD read goes through here (counted for load profiling).
static bool cdRead(uint32_t lba, uint32_t count, void *dst) {
    lsClose(); // one CD action at a time
    uint32_t t0 = g_ps1GPU->now();
    bool ok     = g_ps1CDRom->readSectorsBlocking(lba, count, dst, *g_ps1GPU);
    countRequest(lba, count, g_ps1GPU->now() - t0);
    return ok;
}

/* Load stream (phase 9). PCSX-Redux charges 30 sector times (0.2 s) after every Setloc + ReadN, and a
 * real drive seeks, so a request per 64 KB window made loads request-bound (tools/load_profile.sh:
 * R11A = 29 requests, 9.7 s). During a bracketed load (PS1LoadingActive) window misses start one
 * continuous ReadN instead: the CD IRQ writes each sector into the window being filled and, when that
 * one is full, carries on into the least recently used window the reader is neither using nor has
 * still to read (else it pauses). Sectors are read whole (Setmode 924h size, psx-spx cdromdrive.md
 * "Setmode") and each header's MSF is checked against the expected LBA before the data is DMA'd:
 * a stale or skipped sector can never shift a file (the reverted first attempt's wrong data). A read
 * elsewhere (another file group, GameConfig again) restarts the stream there: the one seek a request
 * cost before. Every other CD user (cdRead, XA music, video) stops it first (PS1FsStopStream). */
volatile uint32_t g_ps1FsStreamStarts  = 0; // stream (re)starts (each is one Setloc + ReadN)
volatile uint32_t g_ps1FsStreamStale   = 0; // sectors whose header was older than expected (dropped)
volatile uint32_t g_ps1FsStreamSkipped = 0; // headers past the expected LBA (stream stopped, restarted)
volatile uint32_t g_ps1FsStreamErrors  = 0; // drive errors (the read falls back to a blocking request)

namespace {

enum class LsState : uint8_t { IDLE, SETLOC, SETMODE, READ, READ_ACK, PAUSE, PAUSE_ACK };

struct LoadStream {
    volatile uint32_t next   = 0;     // LBA the stream delivers next
    volatile int win         = -1;    // window being filled
    volatile bool running    = false; // an action is registered (until its callback ran)
    volatile bool stopping   = false;
    volatile bool error      = false;
    bool session             = false; // started and not yet accounted (countRequest)
    uint32_t sessionLba      = 0;
    uint32_t sessionT0       = 0;
};
LoadStream s_ls;
volatile int s_curWin = -1; // the window the reader copies from: the IRQ never refills it
uint32_t s_hdr[3];          // sector header (MSF + mode) and subheader

inline uint32_t fromBcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }

inline void cdDma(void *dst, uint32_t words) {
    DMA_CTRL[DMA_CDROM].MADR = reinterpret_cast<uintptr_t>(dst);
    DMA_CTRL[DMA_CDROM].BCR  = words | 0x10000;
    DMA_CTRL[DMA_CDROM].CHCR = 0x11000000; // burst: completes before the CPU continues
}

// The next window for the stream: least recently used, not the one it just filled, not the reader's,
// not one filled ahead and still unread.
int pickNext(int cur) {
    int best = -1;
    for (int i = 0; i < WINDOWS; ++i) {
        Window &w = s_win[i];
        if (i == cur || i == s_curWin || !w.buf || w.ahead)
            continue;
        if (best < 0 || w.age < s_win[best].age)
            best = i;
    }
    return best;
}

class LoadStreamAction : public psyqo::CDRomDevice::Action<LsState> {
  public:
    LoadStreamAction() : Action("PS1FsLoadStream") {}
    void start(uint32_t lba) {
        registerMe(g_ps1CDRom);
        setCallback([](bool ok) {
            if (!ok)
                s_ls.error = true;
            s_ls.running = false;
            eastl::atomic_signal_fence(eastl::memory_order_release);
        });
        setState(LsState::SETLOC);
        psyqo::MSF msf(lba + 150);
        uint8_t bcd[3];
        msf.toBCD(bcd);
        psyqo::Hardware::CDRom::Command.send(psyqo::Hardware::CDRom::CDL::SETLOC, bcd[0], bcd[1], bcd[2]);
    }
    bool dataReady(const psyqo::CDRomDevice::Response &) override {
        if (getState() != LsState::READ_ACK)
            return false; // a late sector while pausing
        // Select the sector buffer (as psyqo's readSectors), then read the 12-byte header first.
        psyqo::Hardware::CDRom::Ctrl.throwAway();
        psyqo::Hardware::CDRom::DataRequest = 0;
        psyqo::Hardware::CDRom::InterruptControl.throwAway();
        psyqo::Hardware::CDRom::DataRequest = 0x80;
        psyqo::Hardware::SBus::Dev5Ctrl = 0x20943;
        psyqo::Hardware::SBus::ComCtrl  = 0x132c;
        eastl::atomic_signal_fence(eastl::memory_order_acquire);
        if (s_ls.stopping)
            return pause();
        cdDma(s_hdr, 3);
        const uint8_t *h = reinterpret_cast<const uint8_t *>(s_hdr);
        uint32_t lba     = (fromBcd(h[0]) * 60 + fromBcd(h[1])) * 75 + fromBcd(h[2]) - 150;
        if (lba != s_ls.next) {
            if (lba < s_ls.next) {
                g_ps1FsStreamStale = g_ps1FsStreamStale + 1; // an older sector again: the next one follows
                return false;
            }
            g_ps1FsStreamSkipped = g_ps1FsStreamSkipped + 1; // one was missed: the reader restarts at next
            return pause();
        }
        Window &w = s_win[s_ls.win];
        cdDma(w.buf + w.count * ISO_SECTOR, ISO_SECTOR / 4);
        w.count   = w.count + 1;
        s_ls.next = s_ls.next + 1;
        if (s_ls.next >= s_volumeEnd)
            return pause();
        if (w.count >= WINDOW_SECTORS) {
            int n = pickNext(s_ls.win);
            if (n < 0)
                return pause();
            Window &x = s_win[n];
            x.count   = 0;
            x.lba     = s_ls.next;
            x.ahead   = true;
            x.age     = ++s_winClock;
            s_ls.win  = n;
        }
        eastl::atomic_signal_fence(eastl::memory_order_release);
        return false;
    }
    bool complete(const psyqo::CDRomDevice::Response &) override {
        if (getState() != LsState::PAUSE_ACK)
            return false;
        setSuccess(true);
        return true;
    }
    bool acknowledge(const psyqo::CDRomDevice::Response &) override {
        switch (getState()) {
            case LsState::SETLOC:
                setState(LsState::SETMODE);
                psyqo::Hardware::CDRom::Command.send(psyqo::Hardware::CDRom::CDL::SETMODE, 0xA0); // 2x, 924h sectors
                break;
            case LsState::SETMODE:
                setState(LsState::READ);
                psyqo::Hardware::CDRom::Command.send(psyqo::Hardware::CDRom::CDL::READN);
                break;
            case LsState::READ:
                setState(LsState::READ_ACK);
                break;
            case LsState::PAUSE:
                setState(LsState::PAUSE_ACK);
                break;
            default:
                break;
        }
        return false;
    }

  private:
    bool pause() {
        setState(LsState::PAUSE);
        psyqo::Hardware::CDRom::Command.send(psyqo::Hardware::CDRom::CDL::PAUSE);
        eastl::atomic_signal_fence(eastl::memory_order_release);
        return false;
    }
};

LoadStreamAction s_lsAction;

void lsWaitIdle() {
    eastl::atomic_signal_fence(eastl::memory_order_acquire);
    while (s_ls.running) {
        g_ps1GPU->pumpCallbacks();
        eastl::atomic_signal_fence(eastl::memory_order_acquire);
    }
}

// A finished stream session is profiled as one request (its start LBA and the sectors it delivered).
void lsAccount() {
    if (!s_ls.session || s_ls.running)
        return;
    s_ls.session = false;
    countRequest(s_ls.sessionLba, s_ls.next - s_ls.sessionLba, g_ps1GPU->now() - s_ls.sessionT0);
}

void lsStart(uint32_t lba, int win) {
    Window &w = s_win[win];
    w.count   = 0;
    w.lba     = lba;
    w.ahead   = true;
    w.age     = ++s_winClock;
    s_ls.win      = win;
    s_ls.next     = lba;
    s_ls.error    = false;
    s_ls.stopping = false;
    s_ls.running  = true;
    s_ls.session    = true;
    s_ls.sessionLba = lba;
    s_ls.sessionT0  = g_ps1GPU->now();
    g_ps1FsStreamStarts = g_ps1FsStreamStarts + 1;
    eastl::atomic_signal_fence(eastl::memory_order_release);
    s_lsAction.start(lba);
}

} // namespace

static void lsClose() {
    if (s_ls.running) {
        s_ls.stopping = true;
        eastl::atomic_signal_fence(eastl::memory_order_release);
        lsWaitIdle(); // the next sector pauses the drive
    }
    lsAccount();
    s_ls.win = -1;
    for (auto &w : s_win) w.ahead = false;
}

void PS1FsStopStream() { lsClose(); }

static bool readSector(uint32_t lba) {
    int victim = 0;
    for (int i = 0; i < DIR_CACHE; ++i) {
        if (s_dirCacheAge[i] && s_dirCacheLba[i] == lba) {
            s_dirCacheAge[i] = ++s_dirCacheClock;
            s_sec            = s_dirCache[i];
            g_ps1FsDirHits   = g_ps1FsDirHits + 1;
            return true;
        }
        if (s_dirCacheAge[i] < s_dirCacheAge[victim])
            victim = i;
    }
    if (!cdRead(lba, 1, s_dirCache[victim])) {
        s_dirCacheAge[victim] = 0;
        return false;
    }
    s_dirCacheLba[victim] = lba;
    s_dirCacheAge[victim] = ++s_dirCacheClock;
    s_sec                 = s_dirCache[victim];
    g_ps1FsDirReads       = g_ps1FsDirReads + 1;
    return true;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Case-insensitive compare of a bounded path component `name` (nameLen bytes)
 * vs an ISO9660 entry name of entryLen bytes (stored uppercase, may carry a
 * ";1" version suffix). */
static bool nameMatchesBounded(const char *name, int nameLen, const uint8_t *entryName, int entryLen) {
    int i = 0, j = 0;
    while (i < nameLen && j < entryLen) {
        char e = (char)entryName[j];
        if (e == ';')
            return i == nameLen; /* version suffix -> component must end here */
        char lo = name[i];
        if (lo >= 'A' && lo <= 'Z') lo += 'a' - 'A';
        char el = e;
        if (el >= 'A' && el <= 'Z') el += 'a' - 'A';
        if (lo != el)
            return false;
        ++i; ++j;
    }
    return i == nameLen; /* component fully consumed */
}

/* Find a file by a nested path (e.g. "Data/Stages/SSZ/Act1.bin"), walking the
 * ISO9660 directory tree. Fills lba/size. Returns false if not found. */
static bool findEntry(const char *path, uint32_t *outLba, uint32_t *outSize, bool wantDir) {
    if (!readSector(16)) /* Primary Volume Descriptor */
        return false;
    s_volumeEnd = le32(s_sec + 80); /* volume space size */
    /* Root directory record is at PVD offset 156. */
    uint32_t dirLba  = le32(s_sec + 156 + 2);
    uint32_t dirSize = le32(s_sec + 156 + 10);

    const char *p = path;
    while (true) {
        const char *name = p;
        while (*p && *p != '/') ++p;
        int  nameLen = (int)(p - name);
        bool last    = (*p == 0);
        if (*p == '/') ++p;

        if (nameLen == 0) { /* empty component (leading/trailing slash) */
            if (last) return false;
            continue;
        }

        bool     found     = false;
        uint32_t remaining = dirSize;
        uint32_t sector    = dirLba;
        while (remaining > 0 && !found) {
            if (!readSector(sector))
                return false;
            const uint8_t *q   = s_sec;
            const uint8_t *end = s_sec + ISO_SECTOR;
            while (q < end) {
                uint8_t recLen = q[0];
                if (recLen == 0) { /* padding to sector boundary */
                    remaining -= (uint32_t)(end - q);
                    break;
                }
                if (recLen < 34) /* malformed */
                    return false;
                uint8_t nlen = q[32];
                if (nameMatchesBounded(name, nameLen, q + 33, nlen)) {
                    uint32_t lba   = le32(q + 2);
                    uint32_t sz    = le32(q + 10);
                    uint8_t  flags = q[25];
                    bool     isDir = (flags & 2) != 0;
                    if (last) {
                        if (isDir != wantDir) return false; /* file vs directory */
                        *outLba = lba; *outSize = sz;
                        return true;
                    }
                    if (!isDir) return false; /* expecting a directory */
                    dirLba = lba; dirSize = sz;
                    found = true;
                    break;
                }
                q += recLen;
                remaining -= recLen;
            }
            ++sector;
        }
        if (!found)
            return false;
    }
}

/* File index (phase 9): tools/disc/build_disc.py writes PS1FILES.IDX at the root after mastering:
 * 'FIX1', u32 count, 8 bytes 0, then {u32 FNV-1a 32 of the lowercase path, u32 LBA, u32 size} sorted
 * by hash, every file under Data/. Loaded at the first fopen and kept (~14 KB): opening a file then
 * needs no directory sector, which cost a seek and stopped the load stream (10 of R11A's 15 stream
 * restarts). A disc without it falls back to the directory walk. */
struct IndexEntry {
    uint32_t hash, lba, size;
};
static IndexEntry *s_index     = nullptr;
static uint32_t    s_indexCount = 0;
static bool        s_indexTried = false;
volatile uint32_t g_ps1FsIndexHits = 0; // fopen resolved by the index (GDB)

static uint32_t pathHash(const char *p) {
    uint32_t h = 0x811C9DC5u;
    for (; *p; ++p) {
        uint8_t c = (uint8_t)*p;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        else if (c == '\\')
            c = '/';
        h = (h ^ c) * 0x01000193u;
    }
    return h;
}

static void loadIndex() {
    s_indexTried = true;
    uint32_t lba, size;
    if (!findEntry("PS1FILES.IDX", &lba, &size, false) || size < 16)
        return;
    uint32_t sectors = (size + ISO_SECTOR - 1) / ISO_SECTOR;
    uint8_t *buf     = (uint8_t *)malloc(sectors * ISO_SECTOR);
    if (!buf)
        return;
    uint32_t count = 0;
    if (!cdRead(lba, sectors, buf) || memcmp(buf, "FIX1", 4) || 16 + 12 * (count = le32(buf + 4)) > size) {
        free(buf);
        return;
    }
    memmove(buf, buf + 16, 12 * count);
    s_index      = (IndexEntry *)buf; // the unused tail of the last sector stays allocated (< 2 KB)
    s_indexCount = count;
}

static bool findFile(const char *path, uint32_t *outLba, uint32_t *outSize) {
    if (!s_indexTried)
        loadIndex();
    if (s_index) {
        uint32_t h = pathHash(path), lo = 0, hi = s_indexCount;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (s_index[mid].hash < h)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < s_indexCount && s_index[lo].hash == h) {
            *outLba  = s_index[lo].lba;
            *outSize = s_index[lo].size;
            g_ps1FsIndexHits = g_ps1FsIndexHits + 1;
            return true;
        }
        // Every file under Data/ is indexed: a miss there is a file that isn't on the disc.
        if ((path[0] | 32) == 'd' && (path[1] | 32) == 'a' && (path[2] | 32) == 't' && (path[3] | 32) == 'a' && path[4] == '/')
            return false;
    }
    return findEntry(path, outLba, outSize, false);
}
static bool findDir(const char *path, uint32_t *outLba, uint32_t *outSize) { return findEntry(path, outLba, outSize, true); }

bool PS1FsFind(const char *path, uint32_t *lba, uint32_t *size) { return findFile(path, lba, size); }

bool PS1XaStopForFileRead(); // xa_music.cpp

/* Resident files: small files the scripts read during play (LoadPalette), preloaded into RAM
 * so reading them never touches the drive, which may be streaming CD-XA music. */
#define RESIDENT_MAX 16
struct Resident {
    char path[48];
    uint8_t *data;
    uint32_t size;
};
static Resident s_resident[RESIDENT_MAX];
static int s_residentCount = 0;
volatile uint32_t g_ps1FsResidentReads = 0; // fopen served from RAM (GDB)

static bool pathEq(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        char x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

/* Preload every file of `dir` (e.g. "Data/Palettes") up to maxSize bytes. Returns the count. */
int PS1FsPreloadDir(const char *dir, uint32_t maxSize) {
    uint32_t dirLba, dirSize;
    if (!findDir(dir, &dirLba, &dirSize))
        return 0;
    int loaded = 0;
    for (uint32_t off = 0; off < dirSize; off += ISO_SECTOR) {
        if (!readSector(dirLba + off / ISO_SECTOR))
            break;
        static uint8_t sec[ISO_SECTOR]; // s_sec may be evicted by nested reads; not on the stack (2 KB)
        memcpy(sec, s_sec, ISO_SECTOR);
        for (const uint8_t *q = sec; q < sec + ISO_SECTOR && q[0];) {
            uint8_t recLen = q[0], nlen = q[32];
            uint32_t lba = le32(q + 2), sz = le32(q + 10);
            bool isDir = (q[25] & 2) != 0;
            if (!isDir && nlen > 1 && sz <= maxSize && s_residentCount < RESIDENT_MAX) {
                Resident &r = s_resident[s_residentCount];
                int n = snprintf(r.path, sizeof(r.path), "%s/", dir);
                for (int i = 0; i < nlen && q[33 + i] != ';' && n < (int)sizeof(r.path) - 1; ++i) r.path[n++] = (char)q[33 + i];
                r.path[n] = 0;
                r.data = (uint8_t *)malloc(sz ? sz : 1);
                if (r.data && readViaWindows(lba, sz, r.data)) { // contiguous files: few requests
                    r.size = sz;
                    ++s_residentCount;
                    ++loaded;
                }
            }
            q += recLen;
        }
    }
    return loaded;
}

FILE *fopen(const char *path, const char *mode) {
    (void)mode;
    uint32_t lba, size;
    if (!g_ps1CDRom || !g_ps1GPU)
        return nullptr;
    if (PS1VideoIsOpen()) {
        // One CD action at a time: a file read while the FMV streams would break both.
        // Callers stop the video first (PS1StopVideo); this is the safety net.
        ramsyscall_printf("[cdrom_fs] fopen('%s') while a video streams: closing the video\n", path);
        g_ps1VideoForcedClose = g_ps1VideoForcedClose + 1;
        PS1VideoClose();
    }
    for (int i = 0; i < s_residentCount; ++i) {
        if (pathEq(s_resident[i].path, path)) {
            FILE *f = (FILE *)malloc(sizeof(FILE));
            if (!f)
                return nullptr;
            f->lba   = 0;
            f->size  = s_resident[i].size;
            f->pos   = 0;
            f->valid = 1;
            f->mem   = s_resident[i].data;
            g_ps1FsResidentReads = g_ps1FsResidentReads + 1;
            return f;
        }
    }
    // One CD action at a time: CD-XA music reads continuously; scripts stop it before loads,
    // this is the safety net (counted in g_ps1XaForcedStops).
    PS1XaStopForFileRead();
    if (!findFile(path, &lba, &size))
        return nullptr;
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f)
        return nullptr;
    f->lba   = lba;
    f->size  = size;
    f->pos   = 0;
    f->valid  = 1;
    f->mem    = nullptr;
    return f;
}

void PS1FsReleaseWindows() {
    lsClose();
    s_curWin = -1;
    for (auto &w : s_win) {
        free(w.buf);
        w.buf   = nullptr;
        w.lba   = 0;
        w.count = 0;
        w.age   = 0;
        w.ahead = false;
    }
    s_fallback.count = 0;
}

bool PS1LoadingActive(); // loading.cpp

int fclose(FILE *f) {
    if (!f)
        return 0;
    free(f);
    if (!PS1LoadingActive())
        PS1FsReleaseWindows(); // reads outside a bracketed load don't keep the windows' heap
    return 0;
}

// The window holding `sector`, filling the least recently used one on a miss (one CD request of
// up to WINDOW_SECTORS sectors).
static Window *windowFor(uint32_t sector);

static bool readViaWindows(uint32_t lba, uint32_t bytes, uint8_t *dst) {
    for (uint32_t pos = 0; pos < bytes;) {
        Window *w = windowFor(lba + pos / ISO_SECTOR);
        if (!w)
            return false;
        uint32_t skip  = (lba - w->lba) * ISO_SECTOR + pos;
        uint32_t chunk = w->count * ISO_SECTOR - skip;
        if (chunk > bytes - pos)
            chunk = bytes - pos;
        memcpy(dst + pos, w->buf + skip, chunk);
        pos += chunk;
    }
    return true;
}

// One more window buffer for the load stream (its IRQ may move into it once it is visible).
static void growWindows() {
    for (auto &w : s_win) {
        if (w.buf)
            continue;
        uint8_t *buf = (uint8_t *)malloc(WINDOW_SECTORS * ISO_SECTOR);
        if (!buf)
            return;
        w.count = 0;
        w.age   = 0;
        w.ahead = false;
        psyqo::Kernel::fastEnterCriticalSection();
        w.buf = buf;
        psyqo::Kernel::fastLeaveCriticalSection();
        return;
    }
}

// During a load: the window holding `sector` from the load stream, waiting for it when the stream is
// about to deliver it, else restarting the stream there. nullptr: use blocking requests instead.
static Window *streamWindowFor(uint32_t sector) {
    for (int restarts = 0;;) {
        int hit = -1;
        psyqo::Kernel::fastEnterCriticalSection(); // the IRQ may be moving the stream to another window
        for (int i = 0; i < WINDOWS; ++i) {
            Window &w = s_win[i];
            if (w.buf && w.count && sector >= w.lba && sector < w.lba + w.count) {
                hit = i;
                break;
            }
        }
        bool coming = s_ls.running && !s_ls.stopping && s_ls.win >= 0 && sector >= s_ls.next &&
                      sector < s_ls.next + WINDOW_SECTORS;
        if (hit >= 0) {
            s_curWin          = hit;
            s_win[hit].ahead  = false;
            s_win[hit].age    = ++s_winClock;
        } else if (coming) {
            s_curWin = s_ls.win; // the reader waits there: the stream may move on into the others
        }
        psyqo::Kernel::fastLeaveCriticalSection();
        if (hit >= 0) {
            g_ps1FsWindowHits = g_ps1FsWindowHits + 1;
            return &s_win[hit];
        }
        if (coming) {
            // A sequential read (past the session's first sector): give the stream one more window to
            // carry on into, as the blocking path grew one per miss. A one-sector read (the video's
            // sidecar inside PS1FmvBeginFrame's bracket) keeps a single window: the heap is for the video.
            if (sector > s_ls.sessionLba)
                growWindows();
            g_ps1GPU->pumpCallbacks();
            continue;
        }
        lsClose();
        if (s_ls.error || restarts++ >= 4) { // this read goes through a blocking request; the next miss streams again
            s_ls.error          = false;
            g_ps1FsStreamErrors = g_ps1FsStreamErrors + 1;
            return nullptr;
        }
        // The least recently used window, an empty slot first (allocated now, as the blocking path did).
        int victim = -1;
        for (int i = 0; i < WINDOWS; ++i)
            if (victim < 0 || s_win[i].age < s_win[victim].age)
                victim = i;
        if (!s_win[victim].buf && !(s_win[victim].buf = (uint8_t *)malloc(WINDOW_SECTORS * ISO_SECTOR))) {
            victim = -1;
            for (int i = 0; i < WINDOWS; ++i)
                if (s_win[i].buf && (victim < 0 || s_win[i].age < s_win[victim].age))
                    victim = i;
        }
        if (victim < 0)
            return nullptr;
        s_curWin = victim;
        g_ps1FsWindowFills = g_ps1FsWindowFills + 1;
        lsStart(sector, victim);
    }
}

static Window *windowFor(uint32_t sector) {
    if (PS1LoadingActive() && sector < s_volumeEnd) {
        if (Window *w = streamWindowFor(sector))
            return w;
    }
    Window *victim = &s_win[0];
    for (auto &w : s_win) {
        if (w.buf && w.count && sector >= w.lba && sector < w.lba + w.count) {
            w.age            = ++s_winClock;
            g_ps1FsWindowHits = g_ps1FsWindowHits + 1;
            return &w;
        }
        if (w.age < victim->age)
            victim = &w;
    }
    if (!victim->buf && !(victim->buf = (uint8_t *)malloc(WINDOW_SECTORS * ISO_SECTOR))) {
        if (s_fallback.count && sector == s_fallback.lba)
            return &s_fallback;
        s_fallback.count = 0;
        if (!cdRead(sector, 1, s_fallback.buf))
            return nullptr;
        s_fallback.lba   = sector;
        s_fallback.count = 1;
        g_ps1FsFallbackReads = g_ps1FsFallbackReads + 1;
        return &s_fallback;
    }
    uint32_t count = s_volumeEnd > sector ? s_volumeEnd - sector : 1;
    if (count > WINDOW_SECTORS)
        count = WINDOW_SECTORS;
    victim->count = 0;
    if (!cdRead(sector, count, victim->buf))
        return nullptr;
    victim->lba        = sector;
    victim->count      = count;
    victim->age        = ++s_winClock;
    g_ps1FsWindowFills = g_ps1FsWindowFills + 1;
    return victim;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || !f->valid)
        return 0;
    size_t want = size * nmemb;
    if (f->pos >= f->size)
        return 0;
    if (f->pos + want > f->size)
        want = f->size - f->pos;

    uint8_t *dst       = (uint8_t *)ptr;
    size_t   remaining = want;
    if (f->mem) {
        memcpy(dst, f->mem + f->pos, remaining);
        f->pos += (uint32_t)remaining;
        return want / size;
    }
    while (remaining > 0) {
        uint32_t sector = f->lba + (f->pos / ISO_SECTOR);
        uint32_t off    = f->pos % ISO_SECTOR;
        // Big sector-aligned reads (a whole window or more) go straight into the caller's buffer:
        // one request of up to DIRECT_MAX sectors, no copy.
        // (Not during a load: the load stream delivers them without a request each.)
        if (!PS1LoadingActive() && off == 0 && remaining >= WINDOW_SECTORS * ISO_SECTOR && !((uintptr_t)dst & 3)) {
            uint32_t n = (uint32_t)(remaining / ISO_SECTOR);
            if (n > DIRECT_MAX)
                n = DIRECT_MAX;
            if (!cdRead(sector, n, dst))
                break;
            dst += n * ISO_SECTOR;
            f->pos += n * ISO_SECTOR;
            remaining -= n * ISO_SECTOR;
            continue;
        }
        Window *w = windowFor(sector);
        if (!w)
            break;
        uint32_t skip  = (sector - w->lba) * ISO_SECTOR + off;
        uint32_t avail = w->count * ISO_SECTOR - skip;
        uint32_t chunk = (avail < remaining) ? avail : (uint32_t)remaining;
        memcpy(dst, w->buf + skip, chunk);
        dst += chunk;
        f->pos += chunk;
        remaining -= chunk;
    }
    return (want - remaining) / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    (void)ptr;
    (void)size;
    (void)nmemb;
    (void)f;
    return 0; /* read-only disc; saves move to the memory card (Phase 8) */
}

int fseek(FILE *f, long offset, int whence) {
    if (!f || !f->valid)
        return -1;
    long base = 0;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (long)f->pos; break;
        case SEEK_END: base = (long)f->size; break;
        default: return -1;
    }
    long np = base + offset;
    if (np < 0)
        np = 0;
    if ((uint32_t)np > f->size)
        np = (long)f->size;
    f->pos = (uint32_t)np;
    return 0;
}

long ftell(FILE *f) {
    if (!f || !f->valid)
        return -1;
    return (long)f->pos;
}

int fflush(FILE *f) {
    (void)f;
    return 0;
}

int feof(FILE *f) {
    return !f || !f->valid || f->pos >= f->size;
}

int ferror(FILE *f) {
    (void)f;
    return 0;
}

void clearerr(FILE *f) {
    (void)f;
}

int fgetc(FILE *f) {
    unsigned char c;
    if (fread(&c, 1, 1, f) == 1)
        return c;
    return EOF;
}

int fputc(int c, FILE *f) {
    (void)c;
    (void)f;
    return EOF;
}

int getc(FILE *f) { return fgetc(f); }
int putc(int c, FILE *f) { return fputc(c, f); }

int fputs(const char *s, FILE *f) {
    (void)f;
    if (s)
        syscall_puts(s);
    return 0;
}

int puts(const char *s) {
    if (s)
        syscall_puts(s);
    syscall_puts("\n");
    return 0;
}

char *fgets(char *s, int n, FILE *f) {
    if (!s || n <= 0 || !f)
        return nullptr;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            if (i == 0)
                return nullptr;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    s[i] = 0;
    return s;
}

void perror(const char *s) {
    if (s)
        syscall_puts(s);
    syscall_puts("\n");
}
