// RSDKv4 PS1 audio (docs/30 phase 5): RSDKv4/Audio.cpp's functions on the PS1 hardware. Upstream's Audio.cpp (Ogg
// streaming + a software mixer) is not built; this file keeps its state and semantics.
//
// SFX (5.1): SPU-ADPCM samples in banks built by tools/audio/build_sfx.py. Sonic 2's global SFX don't fit SPU RAM
// together, so each stage has its own bank with every SFX its scripts can play (tools/audio/sfx_sets.py): SPU RAM =
// the resident bank (the pause menu's sounds, uploaded at boot) + the stage's (uploaded by LoadStageFiles) or, while
// the menu screens run, the menus' bank. LoadSfx only records the path; a sample is playable once a bank set its
// sfxList entry. RSDK channel c plays on SPU voice c + 1 (voices 1 and 3 have SPU capture buffers: evidence).
//
// Music (5.2: CD-XA): the engine's state only until then.
#include "RetroEngine.hpp"

#include <stdio.h>

#include <psyqo/alloc.h>

#include "spu.hh"

int globalSFXCount = 0;
int stageSFXCount  = 0;

int masterVolume  = MAX_VOLUME;
int trackID       = -1;
int sfxVolume     = MAX_VOLUME;
int bgmVolume     = MAX_VOLUME;
bool audioEnabled = false;

bool musicEnabled = true;
int musicStatus   = MUSIC_STOPPED;
int musicStartPos = 0;
int musicPosition = 0;
int musicRatio    = 0;
TrackInfo musicTracks[TRACK_COUNT];
SFXInfo sfxList[SFX_COUNT];
char sfxNames[SFX_COUNT][0x20];

int currentStreamIndex = 0;
StreamInfo streamInfo[STREAMFILE_COUNT];

ChannelInfo sfxChannels[CHANNEL_COUNT];

static int s_currentMusicTrack = -1;

// ---- SFX banks -------------------------------------------------------------------------------------

volatile uint32_t g_ps1SfxBanks       = 0; // banks uploaded since boot (GDB)
volatile uint32_t g_ps1SfxBankBytes   = 0; // bytes of the last bank
volatile uint32_t g_ps1SfxPlays       = 0; // voice key-ons
volatile uint32_t g_ps1SfxUnloaded    = 0; // PlaySfx of an id no bank provides (a gap in sfx_sets.py's sets)
volatile int32_t g_ps1SfxUnloadedLast = -1;
volatile uint32_t g_ps1SfxNoChannel   = 0; // PlaySfx with every channel busy (upstream indexes channel -1)
volatile uint32_t g_ps1SfxBankErrors  = 0; // bank missing / bad / over SPU RAM

static uint32_t s_spuStageBase = PS1_SPU_SAMPLE_START; // end of the resident bank
static bool s_resident[SFX_COUNT];

struct BankEntry {
    uint16_t id, pad;
    uint32_t offset, size, rate;
};
static_assert(sizeof(BankEntry) == 16, "PS1Sfx.bin entry");

// Reads a bank into SPU RAM at `base` and points its ids' sfxList entries there. Returns the end address.
static uint32_t PS1SfxLoadBank(const char *path, uint32_t base, bool resident)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        g_ps1SfxBankErrors = g_ps1SfxBankErrors + 1;
        return base;
    }
    static BankEntry s_table[SFX_COUNT];
    char magic[4];
    uint16_t head[2] = { 0, 0 };
    bool ok = fread(magic, 1, 4, f) == 4 && magic[0] == 'P' && magic[1] == 'S' && magic[2] == 'F' && magic[3] == 'X'
              && fread(head, 2, 2, f) == 2 && head[0] <= SFX_COUNT
              && fread(s_table, sizeof(BankEntry), head[0], f) == head[0];
    uint32_t dataSize = 0;
    for (int i = 0; ok && i < head[0]; ++i) {
        uint32_t end = s_table[i].offset + s_table[i].size;
        dataSize     = end > dataSize ? end : dataSize;
        ok           = s_table[i].id < SFX_COUNT && (s_table[i].offset % 64) == 0 && (s_table[i].size % 64) == 0;
    }
    ok = ok && base + dataSize <= PS1_SPU_SAMPLE_END;
    const uint32_t chunkMax = 8192;
    uint8_t *buf            = ok && dataSize ? (uint8_t *)psyqo_malloc(chunkMax) : nullptr;
    ok                      = ok && (buf || !dataSize);
    for (uint32_t done = 0; ok && done < dataSize; done += chunkMax) {
        uint32_t n = dataSize - done < chunkMax ? dataSize - done : chunkMax;
        ok         = fread(buf, 1, n, f) == n;
        if (ok)
            PS1SpuWrite(base + done, buf, n);
    }
    if (buf)
        psyqo_free(buf);
    fclose(f);
    if (!ok) {
        g_ps1SfxBankErrors = g_ps1SfxBankErrors + 1;
        return base;
    }
    for (int i = 0; i < head[0]; ++i) {
        SFXInfo &s = sfxList[s_table[i].id];
        s.spuAddr  = base + s_table[i].offset;
        s.rate     = s_table[i].rate;
        s.length   = s_table[i].size;
        s.buffer   = nullptr;
        s.loaded   = true;
        if (resident)
            s_resident[s_table[i].id] = true;
    }
    g_ps1SfxBanks     = g_ps1SfxBanks + 1;
    g_ps1SfxBankBytes = dataSize;
    return base + dataSize;
}

// The stage area's samples are gone (ReleaseStageSfx, a new bank): every non-resident id, global ones included.
void PS1SfxReleaseStage()
{
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        if (sfxChannels[c].sfxID >= 0 && !s_resident[sfxChannels[c].sfxID])
            PS1SfxChannelStop(c);
    }
    for (int i = 0; i < SFX_COUNT; ++i) {
        if (!s_resident[i])
            sfxList[i].loaded = false;
    }
}

void PS1SfxLoadStageBank()
{
    char path[0x60];
    StrCopy(path, "Data/Stages/");
    StrAdd(path, stageList[activeStageList][stageListPosition].folder);
    StrAdd(path, "/PS1Sfx.bin");
    PS1SfxReleaseStage();
    PS1SfxLoadBank(path, s_spuStageBase, false);
}

void PS1SfxLoadMenuBank()
{
    StopAllSfx();
    PS1SfxReleaseStage();
    PS1SfxLoadBank("Data/Game/PS1SfxMenu.bin", s_spuStageBase, false);
}

void PS1SfxChannelStop(int channel) { PS1SpuVoiceStop(1u << (channel + 1)); }

// PC mixer (ProcessAudioMixing): the side opposite to the pan is scaled by 1 - |pan| / 100.
static void PS1SfxChannelVolume(int channel, uint16_t *l, uint16_t *r)
{
    const ChannelInfo *ch = &sfxChannels[channel];
    int vol               = 0x3fff * sfxVolume / MAX_VOLUME;
    int volL = vol, volR = vol;
    if (ch->pan < 0)
        volR = vol * (100 + ch->pan) / 100;
    else if (ch->pan > 0)
        volL = vol * (100 - ch->pan) / 100;
    *l = (uint16_t)volL;
    *r = (uint16_t)volR;
}

static void PS1SfxChannelStart(int channel)
{
    const ChannelInfo *ch = &sfxChannels[channel];
    uint16_t l, r;
    PS1SfxChannelVolume(channel, &l, &r);
    PS1SpuVoicePlay(channel + 1, sfxList[ch->sfxID].spuAddr, sfxList[ch->sfxID].rate, l, r);
    g_ps1SfxPlays = g_ps1SfxPlays + 1;
}

// GDB evidence aid (as in the Sonic CD port): set g_ps1SpuDumpLen (<= 64 KB) and g_ps1SpuDumpAddr; the next frame
// reads that SPU RAM range into g_ps1SpuDumpBuf (heap, kept until the next request) and clears g_ps1SpuDumpLen.
volatile uint32_t g_ps1SpuDumpAddr = 0;
volatile uint32_t g_ps1SpuDumpLen  = 0;
uint8_t *volatile g_ps1SpuDumpBuf  = nullptr;
static void PS1SpuDebugService()
{
    if (g_ps1SpuDumpLen) {
        uint32_t n = g_ps1SpuDumpLen > 0x10000 ? 0x10000 : g_ps1SpuDumpLen;
        n          = (n + 63) & ~63u;
        if (g_ps1SpuDumpBuf)
            psyqo_free(g_ps1SpuDumpBuf);
        g_ps1SpuDumpBuf = (uint8_t *)psyqo_malloc(n);
        if (g_ps1SpuDumpBuf)
            PS1SpuRead(g_ps1SpuDumpAddr, g_ps1SpuDumpBuf, n);
        g_ps1SpuDumpLen = 0;
    }
}

void PS1AudioUpdate()
{
    if (!audioEnabled)
        return;
    PS1SpuDebugService();
    // PC mixer: a channel's sample that runs out restarts when it loops, else the channel is freed (StopSfx). On the
    // SPU the voice's envelope drops to 0 at the sample's end block (spu.hh).
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        int id = sfxChannels[c].sfxID;
        if (id < 0 || PS1SpuVoiceEnvelope(c + 1))
            continue;
        if (sfxChannels[c].loopSFX && sfxList[id].loaded)
            PS1SfxChannelStart(c);
        else
            StopSfx(id);
    }
}

int InitAudioPlayback()
{
    PS1SpuInit();
    audioEnabled   = true;
    s_spuStageBase = PS1SfxLoadBank("Data/Game/PS1SfxResident.bin", PS1_SPU_SAMPLE_START, true);
    StopAllSfx(); //"init"
    LoadGlobalSfx();
    return true;
}

// Upstream's (RSDKv4/Audio.cpp): the global SFX names and paths from GameConfig.bin (ids 0..globalSFXCount-1; the
// stage's follow). LoadSfx only records the paths on PS1.
void LoadGlobalSfx()
{
    FileInfo info;
    FileInfo infoStore;
    char strBuffer[0x100];
    byte fileBuffer = 0;
    int fileBuffer2 = 0;

    globalSFXCount = 0;

    if (LoadFile("Data/Game/GameConfig.bin", &info)) {
        infoStore = info;

        FileRead(&fileBuffer, 1);
        FileRead(strBuffer, fileBuffer);

        FileRead(&fileBuffer, 1);
        FileRead(strBuffer, fileBuffer);

        byte buf[3];
        for (int c = 0; c < 0x60; ++c) FileRead(buf, 3);

        // Read Obect Names
        byte objectCount = 0;
        FileRead(&objectCount, 1);
        for (byte o = 0; o < objectCount; ++o) {
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);
        }

        // Read Script Paths
        for (byte s = 0; s < objectCount; ++s) {
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);
        }

        byte varCount = 0;
        FileRead(&varCount, 1);
        for (byte v = 0; v < varCount; ++v) {
            // Read Variable Name
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);

            // Read Variable Value
            FileRead(&fileBuffer2, 4);
        }

        // Read SFX
        FileRead(&fileBuffer, 1);
        globalSFXCount = fileBuffer;
        for (byte s = 0; s < globalSFXCount; ++s) { // SFX Names
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;

            SetSfxName(strBuffer, s);
        }
        for (byte s = 0; s < globalSFXCount; ++s) { // SFX Paths
            FileRead(&fileBuffer, 1);
            FileRead(&strBuffer, fileBuffer);
            strBuffer[fileBuffer] = 0;

            GetFileInfo(&infoStore);
            CloseFile();
            LoadSfx(strBuffer, s);
            SetFileInfo(&infoStore);
        }

        CloseFile();
    }

    for (int i = 0; i < CHANNEL_COUNT; ++i) sfxChannels[i].sfxID = -1;
}

// ---- music (phase 5.2: CD-XA) ------------------------------------------------------------------------

void PS1MusicStop() {}
void PS1MusicPause(bool pause) { (void)pause; }

void SetMusicTrack(const char *filePath, byte trackID, bool loop, uint loopPoint)
{
    TrackInfo *track = &musicTracks[trackID];
    StrCopy(track->fileName, "Data/Music/");
    StrAdd(track->fileName, filePath);
    track->trackLoop = loop;
    track->loopPoint = loopPoint;
}
void SwapMusicTrack(const char *filePath, byte trackID, uint loopPoint, uint ratio)
{
    if (StrLength(filePath) <= 0) {
        StopMusic(true);
    }
    else {
        TrackInfo *track = &musicTracks[trackID];
        StrCopy(track->fileName, "Data/Music/");
        StrAdd(track->fileName, filePath);
        track->trackLoop = true;
        track->loopPoint = loopPoint;
        musicRatio       = ratio;
        PlayMusic(trackID, 1);
    }
}
bool PlayMusic(int track, int musStartPos)
{
    if (track < 0 || track >= TRACK_COUNT || !musicTracks[track].fileName[0]) {
        StopMusic(true);
        return false;
    }
    musicStartPos       = musStartPos;
    s_currentMusicTrack = track;
    musicStatus         = MUSIC_PLAYING;
    return true;
}

// ---- SFX (RSDKv4/Audio.cpp semantics) -------------------------------------------------------------------

void LoadSfx(char *filePath, byte sfxID)
{
    if (sfxID >= SFX_COUNT)
        return;
    StrCopy(sfxList[sfxID].name, filePath); // the sample itself comes with a bank
}

void PlaySfx(int sfx, bool loop)
{
    if ((uint)sfx >= SFX_COUNT)
        return;
    if (!sfxList[sfx].loaded) {
        g_ps1SfxUnloaded      = g_ps1SfxUnloaded + 1;
        g_ps1SfxUnloadedLast = sfx;
        return;
    }
    int sfxChannelID = -1;
    for (int c = 0; c < CHANNEL_COUNT; ++c) {
        if (sfxChannels[c].sfxID == sfx || sfxChannels[c].sfxID == -1) {
            sfxChannelID = c;
            break;
        }
    }
    if (sfxChannelID < 0) { // upstream writes channel -1
        g_ps1SfxNoChannel = g_ps1SfxNoChannel + 1;
        return;
    }
    ChannelInfo *sfxInfo  = &sfxChannels[sfxChannelID];
    sfxInfo->sfxID        = sfx;
    sfxInfo->samplePtr    = sfxList[sfx].buffer;
    sfxInfo->sampleLength = sfxList[sfx].length;
    sfxInfo->loopSFX      = loop;
    sfxInfo->pan          = 0;
    PS1SfxChannelStart(sfxChannelID); // the mixer restarts the sample
}

void SetSfxAttributes(int sfx, int loopCount, sbyte pan)
{
    int sfxChannel = -1;
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (sfxChannels[i].sfxID == sfx) {
            sfxChannel = i;
            break;
        }
    }
    if (sfxChannel == -1)
        return; // wasn't found

    ChannelInfo *sfxInfo = &sfxChannels[sfxChannel];
    sfxInfo->loopSFX     = loopCount == -1 ? sfxInfo->loopSFX : loopCount;
    sfxInfo->pan         = pan;
    sfxInfo->sfxID       = sfx;
    uint16_t l, r; // the playing voice follows the new pan (the mixer applies it from the next buffer)
    PS1SfxChannelVolume(sfxChannel, &l, &r);
    PS1SpuVoiceVolume(sfxChannel + 1, l, r);
}

void SetSfxName(const char *sfxName, int sfxID)
{
    if ((uint)sfxID >= SFX_COUNT)
        return;
    int sfxNameID   = 0;
    int soundNameID = 0;
    while (sfxName[sfxNameID]) {
        if (sfxName[sfxNameID] != ' ')
            sfxNames[sfxID][soundNameID++] = sfxName[sfxNameID];
        ++sfxNameID;
    }
    sfxNames[sfxID][soundNameID] = 0;
}
