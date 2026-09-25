// RSDKv4 PS1 audio — phase 2 (docs/30): headless stand-in for RSDKv4/Audio.cpp.
//
// Keeps the engine's audio state (volumes, track and SFX bookkeeping, SFX names for PlaySfxByName) with no
// sound. Phase 5 replaces this with the SPU driver (ps1/spu.*, per-stage SFX sets pre-converted to SPU-ADPCM)
// and CD-XA music (ps1/xa_music.*), as in the Sonic CD port.
#include "RetroEngine.hpp"

int globalSFXCount = 0;
int stageSFXCount  = 0;

int masterVolume  = MAX_VOLUME;
int trackID       = -1;
int sfxVolume     = MAX_VOLUME;
int bgmVolume     = MAX_VOLUME;
bool audioEnabled = true;

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

int InitAudioPlayback() { return true; }

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

void LoadSfx(char *filePath, byte sfxID)
{
    StrCopy(sfxList[sfxID].name, filePath);
    sfxList[sfxID].loaded = true;
}
void PlaySfx(int sfx, bool loop) {}
void SetSfxAttributes(int sfx, int loopCount, sbyte pan) {}
void SetSfxName(const char *sfxName, int sfxID)
{
    int sfxNameID   = 0;
    int soundNameID = 0;
    while (sfxName[sfxNameID]) {
        if (sfxName[sfxNameID] != ' ')
            sfxNames[sfxID][soundNameID++] = sfxName[sfxNameID];
        ++sfxNameID;
    }
    sfxNames[sfxID][soundNameID] = 0;
}
