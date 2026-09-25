/*
 * PS1 STR video playback (MDEC + VLC).
 *
 * Sonic CD (docs/28 phase 6): full-screen FMV STRs from tools/fmv/build_str.py (320x176, 7-8 video
 * sectors per frame, JP + US XA audio channels), described by an 'STR2' sidecar that also sizes the
 * buffers (max RLE words, max sectors per frame). In direct mode the MDEC output goes column by column
 * (16 x height) straight into VRAM: no full-frame buffer in RAM.
 *
 * Streams a psxavenc `-t strv` file (BS v2, 2048-byte sectors, 10 sectors/frame) with
 * the continuous CD reader (cdrom_stream.hh), Huffman-decodes each frame with
 * DecDCTvlcStart2, then the MDEC hardware does RLE+IDCT+YUV->RGB into a 15bpp BGR555
 * buffer. Every sector's STR header (magic, sector index, frame number) is checked, so
 * a duplicated or missing sector can never shift the bitstream.
 */
#include "video.hh"
#include "mdec.hh"
#include "cdrom_stream.hh"
#include "gpu_chain.hh"

#include <stdio.h>
#include <string.h>

#include <psyqo/alloc.h>
#include <psyqo/gpu.hh>
#include "common/hardware/counters.h"

extern psyqo::GPU *g_ps1GPU;

#define ISO_SECTOR            2048
#define STR_SECTORS_PER_FRAME 10
#define STR_BS_CHUNK          (ISO_SECTOR - 32)                     // 2016
#define STR_BS_MAX            (STR_SECTORS_PER_FRAME * STR_BS_CHUNK) // 20160
#define STR_MAGIC             0x0160
#define STR_RING_SECTORS      20                                    // 40 KB read-ahead (2 frames)

// MDEC RLE output per frame = BS header mdec0 low 16 bits (+1 command word). Measured
// over all 934 frames of Intro.str: max 13,056 words. 0x3800 = 14,336 words (+9.8%);
// frames that would exceed it are skipped (counted in g_ps1VideoRleOverflow).
#define STR_MAX_RLE_WORDS 0x3800
#define STR_PIXELS        (256 * 160)
#define STR_COLUMN        16 // MDEC macroblock column width (direct mode)

volatile uint32_t g_ps1VideoFrames = 0; // frames decoded
volatile uint32_t g_ps1VideoResync = 0; // sectors skipped because their STR header didn't match
volatile uint32_t g_ps1VideoRleOverflow = 0; // frames skipped: RLE bigger than the buffer
volatile uint32_t g_ps1VideoHeapBytes   = 0; // bytes currently allocated for FMV playback
// Decode cost of the last frame, in hblanks (root counter 1, free-running under psyqo:
// 63.6 us each, ~262 per NTSC frame).
volatile uint32_t g_ps1VideoVlcHbl  = 0; // Huffman (VLC) decode on the CPU
volatile uint32_t g_ps1VideoMdecHbl = 0; // MDEC in/out DMA + wait

uint32_t PS1Hblanks() { return COUNTERS[1].value; }
uint32_t PS1HblanksSince(uint32_t t0) { return (PS1Hblanks() - t0) & 0xffff; }

static FILE *s_str = nullptr;

// All FMV buffers live on the heap only while a video is open (docs/26 B1): ~235 KB that
// the game doesn't carry after the intro. Allocated largest-first to limit fragmentation.
static uint16_t   *s_video    = nullptr; // decoded 15bpp BGR555 frame (80 KB); direct mode: one 16-px column
static uint8_t    *s_ring     = nullptr; // CD read-ahead ring (40 KB)
static uint32_t   *s_rle      = nullptr; // VLC output: MDEC(1) cmd + RLE halfwords (56 KB)
static DECDCTTAB  *s_vlcTable = nullptr; // Huffman table (34 KB)
static uint8_t    *s_bs       = nullptr; // header-stripped BS payload (20 KB)
static VLC_Context s_vlcCtx;

static int s_width = 256;
static int s_height = 160;
static uint32_t s_rleWords = STR_MAX_RLE_WORDS; // RLE buffer size (words)
static uint32_t s_bsBytes  = STR_BS_MAX;        // BS payload buffer size
static bool s_direct = false;                   // MDEC columns straight to VRAM at (s_dstX, s_dstY)
static int s_dstX = 0, s_dstY = 0;
static int s_frameCount = 0;
static bool s_xa        = false; // STR with interleaved XA audio (sidecar .sti): never pauses, drops unrequested frames
static int s_nextFrame  = 1; // STR frame numbers are 1-based
static int s_requested  = 0; // frames asked for by the game, not decoded yet
// Game-side decode pipeline state (see PS1VideoService).
enum { STAGE_EMPTY, STAGE_VLC, STAGE_MDEC };
static int      s_stage  = STAGE_EMPTY; // state of s_bs/s_rle (the frame being decoded)
static bool     s_pixReady = false;     // s_video holds a decoded frame not yet handed over
static uint32_t s_rlePos = 0; // RLE data words written so far (s_rle[1..s_rlePos])
static uint32_t s_vlcAcc = 0; // hblanks spent on this frame's VLC so far

struct StrSectorHeader {
    uint16_t magic;
    uint16_t type;
    uint16_t sectorIndex;
    uint16_t sectorCount;
    uint32_t frame;
    uint32_t bytesUsed;
};

static const StrSectorHeader *hdr(uint32_t i) { return (const StrSectorHeader *)PS1StreamSector(i); }

bool PS1VideoInit() {
    DecDCTReset(0);
    return true;
}

static void freeBuffers() {
    psyqo_free(s_bs);
    psyqo_free(s_vlcTable);
    psyqo_free(s_rle);
    psyqo_free(s_ring);
    psyqo_free(s_video);
    s_bs = nullptr;
    s_vlcTable = nullptr;
    s_rle = nullptr;
    s_ring = nullptr;
    s_video = nullptr;
    g_ps1VideoHeapBytes = 0;
}

static bool allocBuffers() {
    uint32_t pixels = s_direct ? STR_COLUMN * s_height : STR_PIXELS;
    s_ring     = (uint8_t *)psyqo_malloc(STR_RING_SECTORS * ISO_SECTOR);
    s_rle      = (uint32_t *)psyqo_malloc(s_rleWords * sizeof(uint32_t));
    s_vlcTable = (DECDCTTAB *)psyqo_malloc(sizeof(DECDCTTAB));
    s_bs       = (uint8_t *)psyqo_malloc(s_bsBytes);
    s_video    = (uint16_t *)psyqo_malloc(pixels * sizeof(uint16_t));
    if (!s_video || !s_ring || !s_rle || !s_vlcTable || !s_bs) {
        freeBuffers();
        return false;
    }
    g_ps1VideoHeapBytes = pixels * sizeof(uint16_t) + STR_RING_SECTORS * ISO_SECTOR + s_rleWords * sizeof(uint32_t) +
                          sizeof(DECDCTTAB) + s_bsBytes;
    memset(s_video, 0, pixels * sizeof(uint16_t));
    DecDCTvlcBuild(s_vlcTable);
    return true;
}

// Build-time sidecar next to an STR with XA audio (tools/fmv/build_str.py): X.str -> X.sti =
// 'STR2', u32 frames, u32 video (data) sectors, u8 xa file, u8 JP channel, u8 US channel, u8 0,
// u16 width, u16 height, u16 max RLE words, u16 max sectors of a frame.
struct StrInfo {
    uint32_t frames = 0, videoSectors = 0;
    uint8_t xaFile = 0, xaChannel = 0, xaChannel2 = 0;
    bool audio = false;
    uint16_t width = 256, height = 160, rleWords = STR_MAX_RLE_WORDS, frameSectors = STR_SECTORS_PER_FRAME;
};
static bool readStrInfo(const char *path, StrInfo *info) {
    char sti[96];
    int n = (int)strlen(path);
    if (n < 4 || n >= (int)sizeof(sti))
        return false;
    memcpy(sti, path, n + 1);
    memcpy(sti + n - 3, "sti", 3);
    FILE *f = fopen(sti, "rb");
    if (!f)
        return false;
    uint8_t b[24];
    bool ok = fread(b, 1, 24, f) == 24 && !memcmp(b, "STR2", 4);
    fclose(f);
    if (!ok)
        return false;
    info->frames       = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
    info->videoSectors = b[8] | (b[9] << 8) | (b[10] << 16) | ((uint32_t)b[11] << 24);
    info->xaFile       = b[12];
    info->xaChannel    = b[13];
    info->xaChannel2   = b[14];
    info->audio        = true;
    info->width        = b[16] | (b[17] << 8);
    info->height       = b[18] | (b[19] << 8);
    info->rleWords     = b[20] | (b[21] << 8);
    info->frameSectors = b[22] | (b[23] << 8);
    return info->width && info->width <= 320 && info->height && info->height <= 240 && info->frameSectors <= STR_SECTORS_PER_FRAME;
}

volatile uint32_t g_ps1VideoXa = 0; // 1 while an STR with XA audio streams (GDB)

bool PS1VideoOpen(const char *path, int audio) {
    StrInfo info;
    bool hasInfo = readStrInfo(path, &info);
    s_direct     = hasInfo; // Sonic CD's full-screen STRs: MDEC columns straight to VRAM
    s_width      = hasInfo ? info.width : 256;
    s_height     = hasInfo ? info.height : 160;
    s_rleWords   = hasInfo ? info.rleWords + 16u : STR_MAX_RLE_WORDS;
    s_bsBytes    = hasInfo ? info.frameSectors * STR_BS_CHUNK : STR_BS_MAX;
    s_str = fopen(path, "rb");
    if (!s_str)
        return false;
    if (!allocBuffers()) { // not enough heap: skip the video gracefully
        fclose(s_str);
        s_str = nullptr;
        return false;
    }
    uint32_t sectors = s_str->size / ISO_SECTOR;
    s_frameCount = (int)(sectors / STR_SECTORS_PER_FRAME);
    s_xa         = hasInfo && info.audio;
    if (hasInfo)
        s_frameCount = (int)info.frames;
    g_ps1VideoXa = s_xa ? 1 : 0;
    s_nextFrame  = 1;
    s_requested  = 0;
    s_stage      = STAGE_EMPTY;
    s_pixReady   = false;
    s_rlePos     = 0;
    if (s_xa) // the audio: XA sectors interleaved in the STR, played by the drive (audio 1 = the second channel)
        return PS1StreamOpenXA(s_str->lba, info.videoSectors, s_ring, STR_RING_SECTORS, info.xaFile,
                               audio == 1 ? info.xaChannel2 : info.xaChannel);
    return PS1StreamOpen(s_str->lba, sectors, s_ring, STR_RING_SECTORS);
}

// Takes the next complete frame (10 sectors, headers checked) out of the ring into s_bs.
// 1 = s_bs holds frame s_nextFrame, 0 = not yet (still reading / bad sector / skipped),
// -1 = end of stream or drive error.
static int fetchFrame() {
    PS1StreamPump();
    if (PS1StreamError())
        return -1;
    if (s_nextFrame > s_frameCount)
        return -1;

    // Resync: the next frame must start with sector 0 of frame s_nextFrame.
    while (PS1StreamAvailable()) {
        const StrSectorHeader *h = hdr(0);
        if (h->magic == STR_MAGIC && h->sectorIndex == 0 && (int)h->frame >= s_nextFrame) {
            s_nextFrame = (int)h->frame; // a lost frame is skipped, never misaligned
            break;
        }
        PS1StreamConsume(1);
        g_ps1VideoResync = g_ps1VideoResync + 1;
    }
    // Sectors of this frame: 10 in a video-only STR, 8-9 when XA audio takes 1 sector in 8.
    int frameSectors = PS1StreamAvailable() ? hdr(0)->sectorCount : STR_SECTORS_PER_FRAME;
    if (frameSectors < 1 || frameSectors > STR_SECTORS_PER_FRAME) {
        PS1StreamConsume(1);
        g_ps1VideoResync = g_ps1VideoResync + 1;
        return 0;
    }
    if (PS1StreamAvailable() < (uint32_t)frameSectors)
        return PS1StreamEnded() ? -1 : 0; // still reading — keep showing the current frame

    // All sectors must belong to this frame, in order.
    for (int i = 0; i < frameSectors; i++) {
        const StrSectorHeader *h = hdr(i);
        if (h->magic != STR_MAGIC || h->sectorIndex != i || (int)h->frame != s_nextFrame) {
            PS1StreamConsume(1); // drop the bad head and resync next time
            g_ps1VideoResync = g_ps1VideoResync + 1;
            return 0;
        }
    }

    // Strip the 32-byte sector headers: 2016 payload bytes per sector.
    for (int i = 0; i < frameSectors; i++)
        memcpy(s_bs + i * STR_BS_CHUNK, PS1StreamSector(i) + 32, STR_BS_CHUNK);
    PS1StreamConsume(frameSectors);
    PS1StreamPump();

    // RLE size of this frame (BS header mdec0 low 16 bits) + the MDEC(1) command word.
    if ((((const uint32_t *)s_bs)[0] & 0xffff) + 1 > s_rleWords) {
        g_ps1VideoRleOverflow = g_ps1VideoRleOverflow + 1;
        s_nextFrame++;
        return 0;
    }
    return 1;
}

// Hardware decode of s_rle: RLE + IDCT + YUV->RGB into s_video. Synchronous, so a single
// pixel buffer suffices (the caller uploads it before the next decode).
volatile uint32_t g_ps1VideoSyncFail = 0, g_ps1VideoColMdecHbl = 0, g_ps1VideoColUpHbl = 0; // (debug: direct mode)
static void mdecRun() {
    uint32_t t0 = PS1Hblanks();
    // Bit 15 (STP) set on every texel: MDEC's pure black would otherwise be 0x0000 =
    // transparent, and the additive fade overlay would then hit the background twice.
    DecDCTin(s_rle, DECDCT_MODE_16BPP_BIT15);
    if (s_direct) {
        // The MDEC emits macroblock columns top to bottom: each 16 x height column is contiguous rows.
        for (int x = 0; x < s_width; x += STR_COLUMN) {
            uint32_t tc = PS1Hblanks();
            DecDCTout((uint32_t *)s_video, (size_t)(STR_COLUMN * s_height / 2));
            if (DecDCToutSync(0) < 0)
                g_ps1VideoSyncFail = g_ps1VideoSyncFail + 1;
            g_ps1VideoColMdecHbl = PS1HblanksSince(tc);
            tc = PS1Hblanks();
            g_ps1GPU->uploadToVRAM(s_video, psyqo::Rect{{(int16_t)(s_dstX + x), (int16_t)s_dstY}, {STR_COLUMN, (int16_t)s_height}});
            g_ps1VideoColUpHbl = PS1HblanksSince(tc);
        }
    } else {
        DecDCTout((uint32_t *)s_video, (size_t)(s_width * s_height / 2));
    }
    DecDCToutSync(0);
    g_ps1VideoMdecHbl = PS1HblanksSince(t0);
    s_nextFrame++;
    g_ps1VideoFrames = g_ps1VideoFrames + 1;
}

int PS1VideoNextFrame() {
    if (!s_str)
        return -1;
    int r = fetchFrame();
    if (r <= 0)
        return r;
    // Huffman decode -> MDEC RLE buffer (s_rle[0] = MDEC(1) command word), in one go.
    uint32_t t0 = PS1Hblanks();
    if (DecDCTvlcStart2(&s_vlcCtx, s_rle, 0, (const uint32_t *)s_bs) < 0)
        return -1;
    g_ps1VideoVlcHbl = PS1HblanksSince(t0);
    mdecRun();
    return 1;
}

void PS1VideoClose() {
    PS1StreamClose();
    s_xa         = false;
    g_ps1VideoXa = 0;
    if (s_str) {
        fclose(s_str);
        s_str = nullptr;
    }
    freeBuffers();
}

bool PS1VideoIsOpen() { return s_str != nullptr; }
int  PS1VideoFrameCount() { return s_frameCount; }

void PS1VideoRequestFrame() {
    // Capped: a stall can't make later frames flash by to "catch up"; the video just runs
    // a frame or two behind the script (the intro has 74 requests of slack after its end).
    if (s_str && s_requested < 2)
        s_requested++;
}

// ---- Game-side pipeline -------------------------------------------------------------
// One frame's VLC (~285 hblanks = 18 ms on the CPU, up to ~700 on busy frames) + MDEC (~77)
// is longer than a game frame, so decoding runs ahead in slices of at most WORK_BUDGET_HBL
// per game frame: fetch -> VLC in chunks -> MDEC into s_video -> handed over when the game
// asks (NextVideoFrame). The VLC of frame N+1 proceeds while frame N waits in s_video; only
// the MDEC (which overwrites s_video) waits for the handover.

// Measured in the intro: game logic + chain building ~55-75 hblanks per frame, so the
// video's share of a game frame (upload + decode, counted from PS1UpdateVideoTexture's
// start) stops starting new slices after 170 and never lets MDEC run past 190 (262 = one
// frame; heaviest measured intro frame at these values: see docs/20).
#define VLC_CHUNK_WORDS 512 // ~11-25 hblanks each
#define WORK_BUDGET_HBL 170
#define MDEC_LIMIT_HBL  190
#define MDEC_EST_HBL    80

volatile uint32_t g_ps1VideoWorkMaxHbl = 0; // longest decode slice spent in one game frame
volatile uint32_t g_ps1VideoLate       = 0; // game frames where a requested frame wasn't decoded yet
volatile uint32_t g_ps1VideoDropped    = 0; // frames skipped (not decoded) so the stream never pauses
volatile uint32_t g_ps1VideoFetchMaxHbl = 0; // slowest fetchFrame (resync + 20 KB copy + pump)

// One VLC chunk appended to s_rle. Every chunk starts with its own MDEC(1) header word; it
// is written over the previous chunk's last data word (saved + restored), so the data stays
// contiguous and a single header covering the whole frame is patched in at the end.
// 0 = frame complete, 1 = more to do, -1 = bad bitstream.
static int vlcChunk() {
    uint32_t t0 = PS1Hblanks();
    int r;
    if (s_rlePos == 0) {
        r = DecDCTvlcStart2(&s_vlcCtx, s_rle, VLC_CHUNK_WORDS + 1, (const uint32_t *)s_bs);
        if (r >= 0)
            s_rlePos = s_rle[0] & 0xffff;
    } else {
        uint32_t *at    = s_rle + s_rlePos;
        uint32_t  saved = *at;
        r               = DecDCTvlcContinue2(&s_vlcCtx, at, VLC_CHUNK_WORDS + 1);
        uint32_t n      = *at & 0xffff;
        *at             = saved;
        if (r >= 0)
            s_rlePos += n;
    }
    if (r == 0)
        s_rle[0] = 0x38000000 | s_rlePos;
    s_vlcAcc += PS1HblanksSince(t0);
    return r;
}

// While the game is waiting for frames, the stream must never fill the ring: a paused
// stream restarts with a seek (~200 ms without data) and at 2x the CD delivers exactly the
// 15 fps the game asks for, so that time is never won back. When the decoder falls behind,
// the oldest buffered frame is dropped instead (STR frames are all intra-coded). When the
// game isn't asking (the intro's fade-in shows frame 1 for ~4 s), pausing is intended.
#define RING_DROP_SECTORS (STR_RING_SECTORS - 4) // ~1.6 game frames of margin at 2.5 sectors/frame

// With XA audio the stream never pauses (the music would stop), so frames are dropped whether
// the game is asking or not: the video follows the stream, which runs in real time like the music.
static void relieveRing() {
    if (s_requested == 0 && !s_xa)
        return;
    while (PS1StreamAvailable() >= RING_DROP_SECTORS) {
        const StrSectorHeader *h = hdr(0);
        if (h->magic == STR_MAGIC && h->sectorIndex == 0 && h->sectorCount >= 1 && h->sectorCount <= STR_SECTORS_PER_FRAME) {
            PS1StreamConsume(h->sectorCount); // fetchFrame's resync accepts the next one
            g_ps1VideoDropped = g_ps1VideoDropped + 1;
        } else {
            PS1StreamConsume(1);
            g_ps1VideoResync = g_ps1VideoResync + 1;
        }
    }
}

int PS1VideoService() {
    if (!s_str)
        return -1;
    // An STR with audio plays at stream speed, like a real FMV player: every decoded frame is
    // shown as soon as it's ready, so the picture follows the stream (and its music) instead of
    // the script's NextVideoFrame cadence; ring pressure drops stale frames. Video-only STRs stay
    // paced by the script (PC semantics).
    if (s_xa && s_requested == 0)
        s_requested = 1;
    PS1StreamPump(); // keep reading ahead even between requests
    relieveRing();
    if (s_pixReady && s_requested > 0) {
        s_requested--;
        s_pixReady = false;
        return 1; // the caller uploads s_video now, then calls PS1VideoDecode()
    }
    if (s_requested > 0 && !s_xa)
        g_ps1VideoLate = g_ps1VideoLate + 1; // the game is waiting for a frame (script-paced STRs)
    return 0;
}

int PS1VideoDecode(uint32_t t0) {
    if (!s_str)
        return -1;
    while (PS1HblanksSince(t0) < WORK_BUDGET_HBL) {
        if (s_stage == STAGE_EMPTY) {
            uint32_t tf = PS1Hblanks();
            int r       = fetchFrame();
            tf          = PS1HblanksSince(tf);
            if (tf > g_ps1VideoFetchMaxHbl)
                g_ps1VideoFetchMaxHbl = tf;
            if (r < 0) { // end of stream: report it once the game asks for a frame past it
                if (s_pixReady || s_requested == 0)
                    break;
                return -1;
            }
            if (r == 0)
                break; // sectors not here yet
            s_rlePos = 0;
            s_vlcAcc = 0;
            s_stage  = STAGE_VLC;
        } else if (s_stage == STAGE_VLC) {
            int r = vlcChunk();
            if (r < 0)
                return -1;
            if (r == 0) {
                g_ps1VideoVlcHbl = s_vlcAcc;
                s_stage          = STAGE_MDEC;
            }
        } else { // STAGE_MDEC
            if (s_pixReady)
                break; // s_video still holds a frame the game hasn't taken yet
            if (PS1HblanksSince(t0) + MDEC_EST_HBL > MDEC_LIMIT_HBL)
                break; // next game frame
            mdecRun();
            s_pixReady = true;
            s_stage    = STAGE_EMPTY;
        }
    }
    relieveRing();
    uint32_t spent = PS1HblanksSince(t0);
    if (spent > g_ps1VideoWorkMaxHbl)
        g_ps1VideoWorkMaxHbl = spent;
    return 0;
}

// Direct mode (full-screen FMV): the next decoded picture goes to VRAM (x, y) during this call, when
// its turn comes. 1 = a new picture was written, 0 = not this frame, -1 = end of stream / error.
volatile uint32_t g_ps1VideoPictures = 0; // pictures written to VRAM (direct mode)
int PS1VideoDecodeTo(uint32_t t0, int x, int y) {
    if (!s_str)
        return -1;
    s_dstX = x;
    s_dstY = y;
    s_requested = 1; // stream-paced: every decoded frame is shown
    PS1StreamPump();
    relieveRing();
    uint32_t before = g_ps1VideoFrames;
    while (PS1HblanksSince(t0) < WORK_BUDGET_HBL) {
        if (s_stage == STAGE_EMPTY) {
            uint32_t tf = PS1Hblanks();
            int r       = fetchFrame();
            tf          = PS1HblanksSince(tf);
            if (tf > g_ps1VideoFetchMaxHbl)
                g_ps1VideoFetchMaxHbl = tf;
            if (r < 0)
                return g_ps1VideoFrames != before ? 1 : -1;
            if (r == 0)
                break;
            s_rlePos = 0;
            s_vlcAcc = 0;
            s_stage  = STAGE_VLC;
        } else if (s_stage == STAGE_VLC) {
            int r = vlcChunk();
            if (r < 0)
                return -1;
            if (r == 0) {
                g_ps1VideoVlcHbl = s_vlcAcc;
                s_stage          = STAGE_MDEC;
            }
        } else { // STAGE_MDEC: one picture per game frame, into this frame's draw buffer
            if (g_ps1VideoFrames != before || PS1HblanksSince(t0) + MDEC_EST_HBL > MDEC_LIMIT_HBL)
                break;
            PS1GpuImmediate(); // the previous frame's chain may still be copying from this buffer
            mdecRun();
            g_ps1VideoPictures = g_ps1VideoPictures + 1;
            s_stage = STAGE_EMPTY;
        }
    }
    relieveRing();
    uint32_t spent = PS1HblanksSince(t0);
    if (spent > g_ps1VideoWorkMaxHbl)
        g_ps1VideoWorkMaxHbl = spent;
    return g_ps1VideoFrames != before ? 1 : 0;
}

uint16_t *PS1VideoPixels() { return s_video; }
int       PS1VideoWidth()  { return s_width; }
int       PS1VideoHeight() { return s_height; }
