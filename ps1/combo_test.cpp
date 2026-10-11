/*
 * 1-up jingle + music event test (`make BOOT=<stage> COMBO=1|2 [GAP=n]`; test harness, make dist refuses it).
 * The tester's freeze (docs/30 / docs/37 "Tester report fixes", 2026-10-11) needs a 1-up jingle and a track started
 * inside its pause window. The GDB runs did it with breakpoints; this does the same with no debugger, for emulators
 * without a GDB stub (RetroArch's PCSX-ReARMed / SwanStation cores):
 *   frame 240:       COMBO=2: the seven emeralds (+ Sonic 1's options.superStates)
 *   frame 300:       rings 99, the first Ring of the stage moved onto the player (the 1-up)
 *   frame 300 + GAP: COMBO=1: the player put just before the Sign Post, running right; COMBO=2: jump, then jump again in the air (Super)
 * The sequence repeats every 1200 frames (lives and the Sign Post permitting) so one boot gives several tries.
 */
#include "RetroEngine.hpp"

extern volatile uint16_t g_ps1PadTestMask;
extern "C" const int g_ps1ComboTest = PS1_COMBO_TEST; // marks the build (make dist refuses it)

#ifndef PS1_COMBO_GAP
#define PS1_COMBO_GAP 30
#endif

static int findType(const char *name)
{
    for (int t = 0; t < OBJECT_COUNT; t++)
        if (StrComp(name, typeNames[t]))
            return t;
    return -1;
}

static Entity *findEntity(int type)
{
    for (int i = 1; i < ENTITY_COUNT; i++)
        if (objectEntityList[i].type == type)
            return &objectEntityList[i];
    return nullptr;
}

static int s_bottom;

void PS1ComboTestTick(uint32_t frame)
{
    if (frame < 240)
        return;
    uint32_t f = (frame - 240) % 1200 + 240;
    uint32_t e = 300 + PS1_COMBO_GAP;
    Entity *player = &objectEntityList[0];
    if (f == 240 && PS1_COMBO_TEST == 2) {
        SetGlobalVariableByName("specialStage.emeralds", 0x7F);
#if PS1_GAME == 1
        SetGlobalVariableByName("options.superStates", 1);
#endif
    }
    if (f == 300) {
        player->values[0] = 99;
        if (Entity *ring = findEntity(findType("Ring"))) {
            ring->xpos = player->xpos;
            ring->ypos = player->ypos;
        }
    }
    if (PS1_COMBO_TEST == 1) {
        // The player is put just before the act's Sign Post, running right (the Sign Post moved to the player spun but
        // never finished the act: it waits for the end of the level's camera)
        if (f == e) {
            if (Entity *sign = findEntity(findType("SignPost"))) { // typeNames drop the spaces
                player->xpos  = sign->xpos - (48 << 16);
                player->ypos  = sign->ypos - (16 << 16);
                player->xvel  = 0x40000;
                player->speed = 0x40000;
                s_bottom   = (sign->ypos >> 16) + 256; // the start area's bottom would kill the player
                cameraXPos = player->xpos >> 16;       // the camera jumps along (it follows 16 px a frame: the Sign
                cameraYPos = player->ypos >> 16;       // Post would stay out of its update range)
            }
        }
        if (f >= e && f < e + 120 && curYBoundary2 < s_bottom) // until the camera has caught up (Scene.cpp boundaries)
            curYBoundary2 = newYBoundary2 = s_bottom;
        if (f == e)
            g_ps1PadTestMask = 0x08;
        if (f == e + 60)
            g_ps1PadTestMask = 0;
    }
    if (PS1_COMBO_TEST == 2) {
        if (f == e || f == e + 14)
            g_ps1PadTestMask = 0x10;
        if (f == e + 6 || f == e + 20)
            g_ps1PadTestMask = 0;
    }
}
