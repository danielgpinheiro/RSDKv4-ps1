// RSDKv4 PS1 renderer — phase 2 (docs/30): headless stand-in for RSDKv4/Drawing.cpp.
//
// Defines what the rest of the engine uses from Drawing.cpp (the link's undefined list), with the state
// kept and every draw call a no-op. The draw subs still run in upstream order (they are game logic: the
// Sonic CD title card releases the player from its draw sub, docs/28 phase 2.3); only the Draw* calls do
// nothing. Phase 3 replaces this with the GPU renderer (ps1/render.cpp from the Sonic CD port). Sprite
// pixels are never in RAM on the PS1 (GFXDATA_SIZE, Sprite.cpp).
#include "RetroEngine.hpp"
#include "../ps1/video.hh"

int SCREEN_XSIZE_CONFIG = DEFAULT_SCREEN_XSIZE;
int SCREEN_XSIZE        = DEFAULT_SCREEN_XSIZE; // 320 (user decision; PC 424)
int SCREEN_CENTERX      = DEFAULT_SCREEN_XSIZE / 2;

float SCREEN_XSIZE_F   = DEFAULT_SCREEN_XSIZE;
float SCREEN_CENTERX_F = DEFAULT_SCREEN_XSIZE / 2;
float SCREEN_YSIZE_F   = SCREEN_YSIZE;
float SCREEN_CENTERY_F = SCREEN_YSIZE / 2;

DrawListEntry drawListEntries[DRAWLAYER_COUNT];

int gfxDataPosition;
GFXSurface gfxSurface[SURFACE_COUNT];
byte graphicData[GFXDATA_SIZE];

// Upstream InitRenderDevice -> SetScreenDimensions -> SetScreenSize: the screen width and what depends on it
// (scroll edges, object activity borders). Left at their static values, every bounds-priority object would
// be judged against a 424 px screen.
int InitRenderDevice()
{
    SCREEN_XSIZE        = DEFAULT_SCREEN_XSIZE;
    SCREEN_CENTERX      = SCREEN_XSIZE / 2;
    SCREEN_XSIZE_F      = SCREEN_XSIZE;
    SCREEN_CENTERX_F    = SCREEN_CENTERX;
    SCREEN_SCROLL_LEFT  = SCREEN_CENTERX - 8;
    SCREEN_SCROLL_RIGHT = SCREEN_CENTERX + 8;
    OBJECT_BORDER_X2    = SCREEN_XSIZE + 0x80;
    OBJECT_BORDER_X4    = SCREEN_XSIZE + 0x20;
    return 1;
}
void FlipScreen() {}
void GenerateBlendLookupTable() {}
void ClearScreen(byte index) {}

extern uint32_t g_ps1TypeDrawTicks[OBJECT_COUNT], g_ps1TypeDrawCalls[OBJECT_COUNT]; // Object.cpp profile
void DrawObjectList(int layer)
{
    int size = drawListEntries[layer].listSize;
    for (int i = 0; i < size; ++i) {
        objectEntityPos = drawListEntries[layer].entityRefs[i];
        int type        = objectEntityList[objectEntityPos].type;
        if (type) {
            uint32_t t0 = PS1ProfTicks();
            if (scriptCode[objectScriptList[type].eventDraw.scriptCodePtr] > 0)
                ProcessScript(objectScriptList[type].eventDraw.scriptCodePtr, objectScriptList[type].eventDraw.jumpTablePtr, EVENT_DRAW);
            g_ps1TypeDrawTicks[type] += (PS1ProfTicks() - t0) & 0xFFFF;
            g_ps1TypeDrawCalls[type]++;
        }
    }
}

// Upstream DrawStageGFX (Drawing.cpp) without the layers: water position, the draw lists in the same order.
void DrawStageGFX()
{
    memset(g_ps1TypeDrawTicks, 0, sizeof(g_ps1TypeDrawTicks));
    memset(g_ps1TypeDrawCalls, 0, sizeof(g_ps1TypeDrawCalls));
    waterDrawPos = waterLevel - yScrollOffset;
    if (waterDrawPos < 0)
        waterDrawPos = 0;
    if (waterDrawPos > SCREEN_YSIZE)
        waterDrawPos = SCREEN_YSIZE;

    if (tLayerMidPoint < 3) {
        DrawObjectList(0);
        DrawObjectList(1);
        DrawObjectList(2);
        DrawObjectList(3);
        DrawObjectList(4);
    }
    else if (tLayerMidPoint < 6) {
        DrawObjectList(0);
        DrawObjectList(1);
        DrawObjectList(2);
        DrawObjectList(3);
        DrawObjectList(4);
    }

    if (tLayerMidPoint < 6) {
        DrawObjectList(5);
#if RETRO_REV03
        if (forceUseScripts || Engine.usingOrigins)
            DrawObjectList(7);
#endif
        DrawObjectList(6);
    }
}
void DrawDebugOverlays() {}

void DrawRectangle(int XPos, int YPos, int width, int height, int R, int G, int B, int A) {}
void DrawTintRectangle(int XPos, int YPos, int width, int height) {}
void DrawScaledTintMask(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                        int sheetID)
{
}

void DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID) {}
void DrawSpriteFlipped(int XPos, int YPos, int width, int height, int sprX, int sprY, int direction, int sheetID) {}
void DrawSpriteScaled(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                      int sheetID)
{
}
void DrawSpriteRotated(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                       int sheetID)
{
}
void DrawSpriteRotozoom(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation, int scale,
                        int sheetID)
{
}
void DrawBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID) {}
void DrawAlphaBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID) {}
void DrawAdditiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID) {}
void DrawSubtractiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID) {}

void DrawObjectAnimation(void *objScr, void *ent, int XPos, int YPos) {}
void DrawFace(void *v, uint color) {}
void DrawFadedFace(void *v, uint color, uint fogColor, int alpha) {}
void DrawTexturedFace(void *v, byte sheetID) {}
void DrawTexturedFaceBlended(void *v, byte sheetID) {}
void DrawTextMenu(void *menu, int XPos, int YPos) {}
