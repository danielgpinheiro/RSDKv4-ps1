#include "RetroEngine.hpp"

int vertexCount = 0;
int faceCount   = 0;

Matrix matFinal = Matrix();
Matrix matWorld = Matrix();
Matrix matView  = Matrix();
Matrix matTemp  = Matrix();

Face faceBuffer[FACEBUFFER_SIZE];
Vertex vertexBuffer[VERTEXBUFFER_SIZE];
Vertex vertexBufferT[VERTEXBUFFER_SIZE];

DrawListEntry3D drawList3D[FACEBUFFER_SIZE];

int projectionX = 136;
int projectionY = 160;
int fogColor    = 0;
int fogStrength = 0;

int faceLineStart[SCREEN_YSIZE];
int faceLineEnd[SCREEN_YSIZE];
int faceLineStartU[SCREEN_YSIZE];
int faceLineEndU[SCREEN_YSIZE];
int faceLineStartV[SCREEN_YSIZE];
int faceLineEndV[SCREEN_YSIZE];

void SetIdentityMatrix(Matrix *matrix)
{
    matrix->values[0][0] = 0x100;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = 0;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = 0x100;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixMultiply(Matrix *matrixA, Matrix *matrixB)
{
    int output[16];

    for (int i = 0; i < 0x10; ++i) {
        uint rowA = i / 4;
        uint rowB = i % 4;
        output[i] = (matrixA->values[rowA][3] * matrixB->values[3][rowB] >> 8) + (matrixA->values[rowA][2] * matrixB->values[2][rowB] >> 8)
                    + (matrixA->values[rowA][1] * matrixB->values[1][rowB] >> 8) + (matrixA->values[rowA][0] * matrixB->values[0][rowB] >> 8);
    }

    for (int i = 0; i < 0x10; ++i) matrixA->values[i / 4][i % 4] = output[i];
}
void MatrixTranslateXYZ(Matrix *matrix, int XPos, int YPos, int ZPos)
{
    matrix->values[0][0] = 0x100;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = 0;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = 0x100;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = XPos;
    matrix->values[3][1] = YPos;
    matrix->values[3][2] = ZPos;
    matrix->values[3][3] = 0x100;
}
void MatrixScaleXYZ(Matrix *matrix, int scaleX, int scaleY, int scaleZ)
{
    matrix->values[0][0] = scaleX;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = 0;
    matrix->values[1][1] = scaleY;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = 0;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = scaleZ;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixRotateX(Matrix *matrix, int rotationX)
{
    int sine   = sin512LookupTable[rotationX & 0x1FF] >> 1;
    int cosine = cos512LookupTable[rotationX & 0x1FF] >> 1;

    matrix->values[0][0] = 0x100;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = 0;
    matrix->values[1][1] = cosine;
    matrix->values[1][2] = sine;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = 0;
    matrix->values[2][1] = -sine;
    matrix->values[2][2] = cosine;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixRotateY(Matrix *matrix, int rotationY)
{
    int sine   = sin512LookupTable[rotationY & 0x1FF] >> 1;
    int cosine = cos512LookupTable[rotationY & 0x1FF] >> 1;

    matrix->values[0][0] = cosine;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = sine;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = -sine;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = cosine;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixRotateZ(Matrix *matrix, int rotationZ)
{
    int sine             = sin512LookupTable[rotationZ & 0x1FF] >> 1;
    int cosine           = cos512LookupTable[rotationZ & 0x1FF] >> 1;
    matrix->values[0][0] = cosine;
    matrix->values[0][1] = 0;
    matrix->values[0][2] = sine;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[1][2] = 0;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = -sine;
    matrix->values[2][1] = 0;
    matrix->values[2][2] = cosine;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void MatrixRotateXYZ(Matrix *matrix, short rotationX, short rotationY, short rotationZ)
{
    int sinX = sin512LookupTable[rotationX & 0x1FF] >> 1;
    int cosX = cos512LookupTable[rotationX & 0x1FF] >> 1;
    int sinY = sin512LookupTable[rotationY & 0x1FF] >> 1;
    int cosY = cos512LookupTable[rotationY & 0x1FF] >> 1;
    int sinZ = sin512LookupTable[rotationZ & 0x1FF] >> 1;
    int cosZ = cos512LookupTable[rotationZ & 0x1FF] >> 1;

    matrix->values[0][0] = (cosZ * cosY >> 8) + (sinZ * (sinY * sinX >> 8) >> 8);
    matrix->values[0][1] = (sinZ * cosY >> 8) - (cosZ * (sinY * sinX >> 8) >> 8);
    matrix->values[0][2] = sinY * cosX >> 8;
    matrix->values[0][3] = 0;

    matrix->values[1][0] = sinZ * -cosX >> 8;
    matrix->values[1][1] = cosZ * cosX >> 8;
    matrix->values[1][2] = sinX;
    matrix->values[1][3] = 0;

    matrix->values[2][0] = (sinZ * (cosY * sinX >> 8) >> 8) - (cosZ * sinY >> 8);
    matrix->values[2][1] = (sinZ * -sinY >> 8) - (cosZ * (cosY * sinX >> 8) >> 8);
    matrix->values[2][2] = cosY * cosX >> 8;
    matrix->values[2][3] = 0;

    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
#if !RETRO_REV00
void MatrixInverse(Matrix *matrix)
{
    double inv[16], det;
    double m[16];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            m[(y << 2) + x] = matrix->values[y][x] / 256.0;
        }
    }

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];

    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];

    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];

    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];

    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];

    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];

    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];

    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];

    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];

    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];

    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];

    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];

    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (det == 0)
        return;

    det = 1.0 / det;

    for (int i = 0; i < 0x10; ++i) inv[i] = (int)((inv[i] * det) * 256);
    for (int i = 0; i < 0x10; ++i) matrix->values[i / 4][i % 4] = inv[i];
}
#endif
#if RETRO_PLATFORM == RETRO_PS1
#include "../ps1/video.hh"
// Scene3D CPU per call (hblanks, the last call; GDB, docs/30 phase 7): the vertex transform, the depth sort, the faces.
volatile uint32_t g_ps1Prof3DTransform = 0, g_ps1Prof3DSort = 0, g_ps1Prof3DDraw = 0, g_ps1Prof3DVertices = 0, g_ps1Prof3DFaces = 0;
volatile uint32_t g_ps1Prof3DProject = 0; // the GTE projection pass (part of Draw)
#endif
void TransformVertexBuffer()
{
#if RETRO_PLATFORM == RETRO_PS1
    uint32_t t0 = PS1Hblanks();
#endif
    matFinal.values[0][0] = matWorld.values[0][0];
    matFinal.values[0][1] = matWorld.values[0][1];
    matFinal.values[0][2] = matWorld.values[0][2];
    matFinal.values[0][3] = matWorld.values[0][3];

    matFinal.values[1][0] = matWorld.values[1][0];
    matFinal.values[1][1] = matWorld.values[1][1];
    matFinal.values[1][2] = matWorld.values[1][2];
    matFinal.values[1][3] = matWorld.values[1][3];

    matFinal.values[2][0] = matWorld.values[2][0];
    matFinal.values[2][1] = matWorld.values[2][1];
    matFinal.values[2][2] = matWorld.values[2][2];
    matFinal.values[2][3] = matWorld.values[2][3];

    matFinal.values[3][0] = matWorld.values[3][0];
    matFinal.values[3][1] = matWorld.values[3][1];
    matFinal.values[3][2] = matWorld.values[3][2];
    matFinal.values[3][3] = matWorld.values[3][3];
    MatrixMultiply(&matFinal, &matView);

    for (int v = 0; v < vertexCount; ++v) {
        int vx = vertexBuffer[v].x;
        int vy = vertexBuffer[v].y;
        int vz = vertexBuffer[v].z;

        vertexBufferT[v].x =
            (vx * matFinal.values[0][0] >> 8) + (vy * matFinal.values[1][0] >> 8) + (vz * matFinal.values[2][0] >> 8) + matFinal.values[3][0];
        vertexBufferT[v].y =
            (vx * matFinal.values[0][1] >> 8) + (vy * matFinal.values[1][1] >> 8) + (vz * matFinal.values[2][1] >> 8) + matFinal.values[3][1];
        vertexBufferT[v].z =
            (vx * matFinal.values[0][2] >> 8) + (vy * matFinal.values[1][2] >> 8) + (vz * matFinal.values[2][2] >> 8) + matFinal.values[3][2];
    }
#if RETRO_PLATFORM == RETRO_PS1
    g_ps1Prof3DTransform = PS1HblanksSince(t0);
    g_ps1Prof3DVertices  = vertexCount;
#endif
}
void TransformVertices(Matrix *matrix, int startIndex, int endIndex)
{
    for (int v = startIndex; v < endIndex; ++v) {
        int vx       = vertexBuffer[v].x;
        int vy       = vertexBuffer[v].y;
        int vz       = vertexBuffer[v].z;
        Vertex *vert = &vertexBuffer[v];
        vert->x      = (vx * matrix->values[0][0] >> 8) + (vy * matrix->values[1][0] >> 8) + (vz * matrix->values[2][0] >> 8) + matrix->values[3][0];
        vert->y      = (vx * matrix->values[0][1] >> 8) + (vy * matrix->values[1][1] >> 8) + (vz * matrix->values[2][1] >> 8) + matrix->values[3][1];
        vert->z      = (vx * matrix->values[0][2] >> 8) + (vy * matrix->values[1][2] >> 8) + (vz * matrix->values[2][2] >> 8) + matrix->values[3][2];
    }
}
#if RETRO_PLATFORM == RETRO_PS1 || defined(PS1_SORT3D_CHECK)
// One buffer for two passes of a frame (docs/30 phase 7): the draw-list sort's scratch, then Draw3DScene's per-vertex
// screen positions (PS1Project3DVertices).
DrawListEntry3D g_ps1Scene3DScratch[FACEBUFFER_SIZE];
#endif
#if RETRO_PLATFORM == RETRO_PS1 || defined(PS1_SORT3D_CHECK)
// Stable LSD radix sort, descending by depth (ties keep their order, like upstream's bubble sort): keys max - depth
// ascending, in as many 9-bit passes as the depth range needs (the half-pipe: 2). Identical order to upstream's:
// tools/scene3d/sort3d_check.sh.
void PS1SortDrawList3D(DrawListEntry3D *list, int count)
{
    if (count < 2)
        return;
    int lo = list[0].depth, hi = list[0].depth;
    for (int i = 1; i < count; ++i) {
        lo = list[i].depth < lo ? list[i].depth : lo;
        hi = list[i].depth > hi ? list[i].depth : hi;
    }
    unsigned range = (unsigned)hi - (unsigned)lo;
    DrawListEntry3D *src = list, *dst = g_ps1Scene3DScratch;
    for (int shift = 0; shift < 32 && (range >> shift); shift += 9) {
        int counts[512];
        memset(counts, 0, sizeof(counts));
        for (int i = 0; i < count; ++i) ++counts[(((unsigned)hi - (unsigned)src[i].depth) >> shift) & 511];
        int at = 0;
        for (int b = 0; b < 512; ++b) {
            int n     = counts[b];
            counts[b] = at;
            at += n;
        }
        for (int i = 0; i < count; ++i) dst[counts[(((unsigned)hi - (unsigned)src[i].depth) >> shift) & 511]++] = src[i];
        DrawListEntry3D *t = src;
        src                = dst;
        dst                = t;
    }
    if (src != list)
        for (int i = 0; i < count; ++i) list[i] = src[i];
}
#endif

void Sort3DDrawList()
{
#if RETRO_PLATFORM == RETRO_PS1
    uint32_t t0 = PS1Hblanks();
#endif
    for (int i = 0; i < faceCount; ++i) {
        drawList3D[i].depth = (vertexBufferT[faceBuffer[i].d].z + vertexBufferT[faceBuffer[i].c].z + vertexBufferT[faceBuffer[i].b].z
                               + vertexBufferT[faceBuffer[i].a].z)
                              >> 2;
        drawList3D[i].faceID = i;
    }

#if RETRO_PLATFORM == RETRO_PS1
    // PS1: the bubble sort below is stable and descending, so any stable descending sort gives the same order: a radix
    // sort (docs/30 phase 7; the half-pipe's 742 faces took 4,266 hblanks bubbled, 220 merge-sorted). Identical order
    // checked on the host: tools/scene3d/sort3d_check.sh.
    PS1SortDrawList3D(drawList3D, faceCount);
    g_ps1Prof3DSort  = PS1HblanksSince(t0);
    g_ps1Prof3DFaces = faceCount;
    return;
#endif

    for (int i = 0; i < faceCount; ++i) {
        for (int j = faceCount - 1; j > i; --j) {
            if (drawList3D[j].depth > drawList3D[j - 1].depth) {
                int faceID               = drawList3D[j].faceID;
                int depth                = drawList3D[j].depth;
                drawList3D[j].faceID     = drawList3D[j - 1].faceID;
                drawList3D[j].depth      = drawList3D[j - 1].depth;
                drawList3D[j - 1].faceID = faceID;
                drawList3D[j - 1].depth  = depth;
            }
        }
    }
}
#if RETRO_PLATFORM == RETRO_PS1
// ps1/render.cpp: every transformed vertex projected once on the GTE (the faces share them), as upstream's
// SCREEN_CENTER +- projection * T / T.z within +-1 px (GTE division and flooring); a vertex the GTE can't do (near the
// camera, off the GTE's screen range) is marked and projected here exactly.
void PS1Project3DVertices(uint32_t *out, int count);
#define PS1_PROJ_MARK (0x8000u)
static inline void PS1ProjectedVertex(const uint32_t *cache, int i, int *x, int *y)
{
    uint32_t p = cache[i];
    if ((p & 0xFFFF) != PS1_PROJ_MARK) {
        *x = (int16_t)(p & 0xFFFF);
        *y = (int16_t)(p >> 16);
    }
    else {
        *x = SCREEN_CENTERX + projectionX * vertexBufferT[i].x / vertexBufferT[i].z;
        *y = SCREEN_CENTERY - projectionY * vertexBufferT[i].y / vertexBufferT[i].z;
    }
}
static_assert(sizeof(g_ps1Scene3DScratch) >= VERTEXBUFFER_SIZE * sizeof(uint32_t), "projection cache");
#endif
#if RETRO_PLATFORM == RETRO_PS1
void PS1FadedRunFlush();                                                                                    // ps1/render.cpp
bool PS1FadedRunAdd(uint32_t pa, uint32_t pb, uint32_t pc, uint32_t pd, uint color, uint fogColor, int alpha); // ps1/render.cpp
#endif
void Draw3DScene(int spriteSheetID)
{
#if RETRO_PLATFORM == RETRO_PS1
    uint32_t t0 = PS1Hblanks();
    uint32_t *proj = (uint32_t *)g_ps1Scene3DScratch; // the sort is done: its scratch holds the screen positions
    PS1Project3DVertices(proj, vertexCount);
    g_ps1Prof3DProject = PS1HblanksSince(t0);
    bool runOpen       = false;
#define PS1_PROJECT_QUAD()                                                                                                                  \
    PS1ProjectedVertex(proj, face->a, &quad[0].x, &quad[0].y);                                                                            \
    PS1ProjectedVertex(proj, face->b, &quad[1].x, &quad[1].y);                                                                            \
    PS1ProjectedVertex(proj, face->c, &quad[2].x, &quad[2].y);                                                                            \
    PS1ProjectedVertex(proj, face->d, &quad[3].x, &quad[3].y);
#endif
    Vertex quad[4];
    for (int i = 0; i < faceCount; ++i) {
        Face *face = &faceBuffer[drawList3D[i].faceID];
#if RETRO_PLATFORM == RETRO_PS1
        if (face->flag == FACE_FLAG_FADED) { // the fast path (ps1/render.cpp PS1FadedRunAdd), same fog as below
            uint32_t pa = proj[face->a], pb = proj[face->b], pc = proj[face->c], pd = proj[face->d];
            // no marked corner: every z > 0 (z <= 0 is marked) and GTE positions
            if ((pa & 0xFFFF) != PS1_PROJ_MARK && (pb & 0xFFFF) != PS1_PROJ_MARK && (pc & 0xFFFF) != PS1_PROJ_MARK
                && (pd & 0xFFFF) != PS1_PROJ_MARK) {
                int fogStr = 0;
                if ((drawList3D[i].depth - 0x8000) >> 8 >= 0)
                    fogStr = (drawList3D[i].depth - 0x8000) >> 8;
                if (fogStr > fogStrength)
                    fogStr = fogStrength;
                PS1FadedRunAdd(pa, pb, pc, pd, face->color, fogColor, 0xFF - fogStr);
                runOpen = true;
                continue;
            }
            if (!(vertexBufferT[face->a].z > 0 && vertexBufferT[face->b].z > 0 && vertexBufferT[face->c].z > 0 && vertexBufferT[face->d].z > 0))
                continue; // upstream's test; the general path below would skip it too
        }
        if (runOpen) { // another kind of face (or an exact-fallback corner): the run ends, the draw order is kept
            PS1FadedRunFlush();
            runOpen = false;
        }
#endif
        memset(quad, 0, 4 * sizeof(Vertex));
        switch (face->flag) {
            default: break;
            case FACE_FLAG_TEXTURED_3D:
                if (vertexBufferT[face->a].z > 0 && vertexBufferT[face->b].z > 0 && vertexBufferT[face->c].z > 0 && vertexBufferT[face->d].z > 0) {
#if RETRO_PLATFORM == RETRO_PS1
                    PS1_PROJECT_QUAD()
#else
                    quad[0].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->a].x / vertexBufferT[face->a].z;
                    quad[0].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->a].y / vertexBufferT[face->a].z;
                    quad[1].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->b].x / vertexBufferT[face->b].z;
                    quad[1].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->b].y / vertexBufferT[face->b].z;
                    quad[2].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->c].x / vertexBufferT[face->c].z;
                    quad[2].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->c].y / vertexBufferT[face->c].z;
                    quad[3].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->d].x / vertexBufferT[face->d].z;
                    quad[3].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->d].y / vertexBufferT[face->d].z;
#endif
                    quad[0].u = vertexBuffer[face->a].u;
                    quad[0].v = vertexBuffer[face->a].v;
                    quad[1].u = vertexBuffer[face->b].u;
                    quad[1].v = vertexBuffer[face->b].v;
                    quad[2].u = vertexBuffer[face->c].u;
                    quad[2].v = vertexBuffer[face->c].v;
                    quad[3].u = vertexBuffer[face->d].u;
                    quad[3].v = vertexBuffer[face->d].v;
                    DrawTexturedFace(quad, spriteSheetID);
                }
                break;
            case FACE_FLAG_TEXTURED_2D:
                if (vertexBufferT[face->a].z >= 0 && vertexBufferT[face->b].z >= 0 && vertexBufferT[face->c].z >= 0
                    && vertexBufferT[face->d].z >= 0) {
                    quad[0].x = vertexBufferT[face->a].x;
                    quad[0].y = vertexBufferT[face->a].y;
                    quad[1].x = vertexBufferT[face->b].x;
                    quad[1].y = vertexBufferT[face->b].y;
                    quad[2].x = vertexBufferT[face->c].x;
                    quad[2].y = vertexBufferT[face->c].y;
                    quad[3].x = vertexBufferT[face->d].x;
                    quad[3].y = vertexBufferT[face->d].y;
                    quad[0].u = vertexBuffer[face->a].u;
                    quad[0].v = vertexBuffer[face->a].v;
                    quad[1].u = vertexBuffer[face->b].u;
                    quad[1].v = vertexBuffer[face->b].v;
                    quad[2].u = vertexBuffer[face->c].u;
                    quad[2].v = vertexBuffer[face->c].v;
                    quad[3].u = vertexBuffer[face->d].u;
                    quad[3].v = vertexBuffer[face->d].v;
                    DrawTexturedFace(quad, spriteSheetID);
                }
                break;
            case FACE_FLAG_COLORED_3D:
                if (vertexBufferT[face->a].z > 0 && vertexBufferT[face->b].z > 0 && vertexBufferT[face->c].z > 0 && vertexBufferT[face->d].z > 0) {
#if RETRO_PLATFORM == RETRO_PS1
                    PS1_PROJECT_QUAD()
#else
                    quad[0].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->a].x / vertexBufferT[face->a].z;
                    quad[0].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->a].y / vertexBufferT[face->a].z;
                    quad[1].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->b].x / vertexBufferT[face->b].z;
                    quad[1].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->b].y / vertexBufferT[face->b].z;
                    quad[2].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->c].x / vertexBufferT[face->c].z;
                    quad[2].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->c].y / vertexBufferT[face->c].z;
                    quad[3].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->d].x / vertexBufferT[face->d].z;
                    quad[3].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->d].y / vertexBufferT[face->d].z;
#endif
                    DrawFace(quad, face->color);
                }
                break;
            case FACE_FLAG_COLORED_2D:
                if (vertexBufferT[face->a].z >= 0 && vertexBufferT[face->b].z >= 0 && vertexBufferT[face->c].z >= 0
                    && vertexBufferT[face->d].z >= 0) {
                    quad[0].x = vertexBufferT[face->a].x;
                    quad[0].y = vertexBufferT[face->a].y;
                    quad[1].x = vertexBufferT[face->b].x;
                    quad[1].y = vertexBufferT[face->b].y;
                    quad[2].x = vertexBufferT[face->c].x;
                    quad[2].y = vertexBufferT[face->c].y;
                    quad[3].x = vertexBufferT[face->d].x;
                    quad[3].y = vertexBufferT[face->d].y;
                    DrawFace(quad, face->color);
                }
                break;
            case FACE_FLAG_FADED:
                if (vertexBufferT[face->a].z > 0 && vertexBufferT[face->b].z > 0 && vertexBufferT[face->c].z > 0 && vertexBufferT[face->d].z > 0) {
#if RETRO_PLATFORM == RETRO_PS1
                    PS1_PROJECT_QUAD()
#else
                    quad[0].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->a].x / vertexBufferT[face->a].z;
                    quad[0].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->a].y / vertexBufferT[face->a].z;
                    quad[1].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->b].x / vertexBufferT[face->b].z;
                    quad[1].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->b].y / vertexBufferT[face->b].z;
                    quad[2].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->c].x / vertexBufferT[face->c].z;
                    quad[2].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->c].y / vertexBufferT[face->c].z;
                    quad[3].x = SCREEN_CENTERX + projectionX * vertexBufferT[face->d].x / vertexBufferT[face->d].z;
                    quad[3].y = SCREEN_CENTERY - projectionY * vertexBufferT[face->d].y / vertexBufferT[face->d].z;
#endif

                    int fogStr = 0;
                    if ((drawList3D[i].depth - 0x8000) >> 8 >= 0)
                        fogStr = (drawList3D[i].depth - 0x8000) >> 8;
                    if (fogStr > fogStrength)
                        fogStr = fogStrength;

                    DrawFadedFace(quad, face->color, fogColor, 0xFF - fogStr);
                }
                break;
            case FACE_FLAG_TEXTURED_C:
                if (vertexBufferT[face->a].z > 0) {
                    // [face->a].uv == sprite center
                    // [face->b].uv == ???
                    // [face->c].uv == sprite extend (how far to each edge X & Y)
                    // [face->d].uv == unused

                    quad[0].x = SCREEN_CENTERX + projectionX * (vertexBufferT[face->a].x - vertexBuffer[face->b].u) / vertexBufferT[face->a].z;
                    quad[0].y = SCREEN_CENTERY - projectionY * (vertexBufferT[face->a].y + vertexBuffer[face->b].v) / vertexBufferT[face->a].z;
                    quad[1].x = SCREEN_CENTERX + projectionX * (vertexBufferT[face->a].x + vertexBuffer[face->b].u) / vertexBufferT[face->a].z;
                    quad[1].y = SCREEN_CENTERY - projectionY * (vertexBufferT[face->a].y + vertexBuffer[face->b].v) / vertexBufferT[face->a].z;
                    quad[2].x = SCREEN_CENTERX + projectionX * (vertexBufferT[face->a].x - vertexBuffer[face->b].u) / vertexBufferT[face->a].z;
                    quad[2].y = SCREEN_CENTERY - projectionY * (vertexBufferT[face->a].y - vertexBuffer[face->b].v) / vertexBufferT[face->a].z;
                    quad[3].x = SCREEN_CENTERX + projectionX * (vertexBufferT[face->a].x + vertexBuffer[face->b].u) / vertexBufferT[face->a].z;
                    quad[3].y = SCREEN_CENTERY - projectionY * (vertexBufferT[face->a].y - vertexBuffer[face->b].v) / vertexBufferT[face->a].z;

                    quad[0].u = vertexBuffer[face->a].u - vertexBuffer[face->c].u;
                    quad[0].v = vertexBuffer[face->a].v - vertexBuffer[face->c].v;
                    quad[1].u = vertexBuffer[face->a].u + vertexBuffer[face->c].u;
                    quad[1].v = vertexBuffer[face->a].v - vertexBuffer[face->c].v;
                    quad[2].u = vertexBuffer[face->a].u - vertexBuffer[face->c].u;
                    quad[2].v = vertexBuffer[face->a].v + vertexBuffer[face->c].v;
                    quad[3].u = vertexBuffer[face->a].u + vertexBuffer[face->c].u;
                    quad[3].v = vertexBuffer[face->a].v + vertexBuffer[face->c].v;

                    DrawTexturedFace(quad, spriteSheetID);
                }
                break;
            case FACE_FLAG_TEXTURED_C_BLEND:
                if (vertexBufferT[face->a].z > 0) {
                    // See above, its the same just blended

                    quad[0].x = SCREEN_CENTERX + projectionX * (vertexBufferT[face->a].x - vertexBuffer[face->b].u) / vertexBufferT[face->a].z;
                    quad[0].y = SCREEN_CENTERY - projectionY * (vertexBufferT[face->a].y + vertexBuffer[face->b].v) / vertexBufferT[face->a].z;
                    quad[1].x = SCREEN_CENTERX + projectionX * (vertexBufferT[face->a].x + vertexBuffer[face->b].u) / vertexBufferT[face->a].z;
                    quad[1].y = SCREEN_CENTERY - projectionY * (vertexBufferT[face->a].y + vertexBuffer[face->b].v) / vertexBufferT[face->a].z;
                    quad[2].x = SCREEN_CENTERX + projectionX * (vertexBufferT[face->a].x - vertexBuffer[face->b].u) / vertexBufferT[face->a].z;
                    quad[2].y = SCREEN_CENTERY - projectionY * (vertexBufferT[face->a].y - vertexBuffer[face->b].v) / vertexBufferT[face->a].z;
                    quad[3].x = SCREEN_CENTERX + projectionX * (vertexBufferT[face->a].x + vertexBuffer[face->b].u) / vertexBufferT[face->a].z;
                    quad[3].y = SCREEN_CENTERY - projectionY * (vertexBufferT[face->a].y - vertexBuffer[face->b].v) / vertexBufferT[face->a].z;

                    quad[0].u = vertexBuffer[face->a].u - vertexBuffer[face->c].u;
                    quad[0].v = vertexBuffer[face->a].v - vertexBuffer[face->c].v;
                    quad[1].u = vertexBuffer[face->a].u + vertexBuffer[face->c].u;
                    quad[1].v = vertexBuffer[face->a].v - vertexBuffer[face->c].v;
                    quad[2].u = vertexBuffer[face->a].u - vertexBuffer[face->c].u;
                    quad[2].v = vertexBuffer[face->a].v + vertexBuffer[face->c].v;
                    quad[3].u = vertexBuffer[face->a].u + vertexBuffer[face->c].u;
                    quad[3].v = vertexBuffer[face->a].v + vertexBuffer[face->c].v;

                    DrawTexturedFaceBlended(quad, spriteSheetID);
                }
                break;
            case FACE_FLAG_3DSPRITE:
                if (vertexBufferT[face->a].z > 0) {
                    int xpos = SCREEN_CENTERX + projectionX * vertexBufferT[face->a].x / vertexBufferT[face->a].z;
                    int ypos = SCREEN_CENTERY - projectionY * vertexBufferT[face->a].y / vertexBufferT[face->a].z;

                    ObjectScript *scriptInfo = &objectScriptList[vertexBuffer[face->a].u];
                    SpriteFrame *frame       = &scriptFrames[scriptInfo->frameListOffset + vertexBuffer[face->b].u];

                    switch (vertexBuffer[face->a].v) {
                        case FX_SCALE:
                            DrawSpriteScaled(vertexBuffer[face->b].v, xpos, ypos, -frame->pivotX, -frame->pivotY, vertexBuffer[face->c].u,
                                             vertexBuffer[face->c].u, frame->width, frame->height, frame->sprX, frame->sprY,
                                             scriptInfo->spriteSheetID);
                            break;
                        case FX_ROTATE:
                            DrawSpriteRotated(vertexBuffer[face->b].v, xpos, ypos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY,
                                              frame->width, frame->height, vertexBuffer[face->c].v, scriptInfo->spriteSheetID);
                            break;
                        case FX_ROTOZOOM:
                            DrawSpriteRotozoom(vertexBuffer[face->b].v, xpos, ypos, -frame->pivotX, -frame->pivotY, frame->sprX, frame->sprY,
                                               frame->width, frame->height, vertexBuffer[face->c].v, vertexBuffer[face->c].u,
                                               scriptInfo->spriteSheetID);
                            break;
                    }
                }
                break;
        }
    }
#if RETRO_PLATFORM == RETRO_PS1
#undef PS1_PROJECT_QUAD
    PS1FadedRunFlush();
    g_ps1Prof3DDraw = PS1HblanksSince(t0);
#endif
}

void ProcessScanEdge(Vertex *vertA, Vertex *vertB)
{
    int bottom, top;

    if (vertA->y == vertB->y)
        return;
    if (vertA->y >= vertB->y) {
        top    = vertB->y;
        bottom = vertA->y + 1;
    }
    else {
        top    = vertA->y;
        bottom = vertB->y + 1;
    }
    if (top > SCREEN_YSIZE - 1 || bottom < 0)
        return;
    if (bottom > SCREEN_YSIZE)
        bottom = SCREEN_YSIZE;
    int fullX  = vertA->x << 16;
    int deltaX = ((vertB->x - vertA->x) << 16) / (vertB->y - vertA->y);
    if (top < 0) {
        fullX -= top * deltaX;
        top = 0;
    }
    for (int i = top; i < bottom; ++i) {
        int trueX = fullX >> 16;
        if (trueX < faceLineStart[i])
            faceLineStart[i] = trueX;
        if (trueX > faceLineEnd[i])
            faceLineEnd[i] = trueX;
        fullX += deltaX;
    }
}
void ProcessScanEdgeUV(Vertex *vertA, Vertex *vertB)
{
    int bottom, top;

    if (vertA->y == vertB->y)
        return;
    if (vertA->y >= vertB->y) {
        top    = vertB->y;
        bottom = vertA->y + 1;
    }
    else {
        top    = vertA->y;
        bottom = vertB->y + 1;
    }
    if (top > SCREEN_YSIZE - 1 || bottom < 0)
        return;
    if (bottom > SCREEN_YSIZE)
        bottom = SCREEN_YSIZE;

    int fullX  = vertA->x << 16;
    int fullU  = vertA->u << 16;
    int fullV  = vertA->v << 16;
    int deltaX = ((vertB->x - vertA->x) << 16) / (vertB->y - vertA->y);

    int deltaU = 0;
    if (vertA->u != vertB->u)
        deltaU = ((vertB->u - vertA->u) << 16) / (vertB->y - vertA->y);

    int deltaV = 0;
    if (vertA->v != vertB->v) {
        deltaV = ((vertB->v - vertA->v) << 16) / (vertB->y - vertA->y);
    }

    if (top < 0) {
        fullX -= top * deltaX;
        fullU -= top * deltaU;
        fullV -= top * deltaV;
        top = 0;
    }
    for (int i = top; i < bottom; ++i) {
        int trueX = fullX >> 16;
        if (trueX < faceLineStart[i]) {
            faceLineStart[i]  = trueX;
            faceLineStartU[i] = fullU;
            faceLineStartV[i] = fullV;
        }
        if (trueX > faceLineEnd[i]) {
            faceLineEnd[i]  = trueX;
            faceLineEndU[i] = fullU;
            faceLineEndV[i] = fullV;
        }
        fullX += deltaX;
        fullU += deltaU;
        fullV += deltaV;
    }
}
