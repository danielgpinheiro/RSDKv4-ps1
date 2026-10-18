#include "RetroEngine.hpp"

int vertexCount = 0;
int faceCount   = 0;

Matrix matFinal = Matrix();
Matrix matWorld = Matrix();
Matrix matView  = Matrix();
Matrix matTemp  = Matrix();

#if RETRO_PLATFORM == RETRO_PS1
#include <psyqo/alloc.h>
#include "../ps1/gpu_chain.hh"
// Without the block (see Scene3D.hpp): 1-entry dummies, capacity 0.
static Face s_ps1DummyFace[1];
static Vertex s_ps1DummyVertex[1];
static PS1ProjEntry s_ps1DummyProj[1];
static PS1ExactEntry s_ps1DummyExact[1]; // PS1ExactT(v) masks v with 255, but without the block every v is 0 (capacity 0)
static DrawListEntry3D s_ps1DummyDraw[1], s_ps1DummyScratch[1];
Face *faceBuffer          = s_ps1DummyFace;
Vertex *vertexBuffer      = s_ps1DummyVertex;
PS1ProjEntry *g_ps1Proj   = s_ps1DummyProj;
PS1ExactEntry *g_ps1ExactT = s_ps1DummyExact;
int g_ps1VtxCap = 0, g_ps1FaceCap = 0;
uint g_ps1ExactFrame = 1;
#define vertexBufferT (PS1VertexTView{}) // upstream's reads below get the exact transform (PS1ExactT)
#else
Vertex vertexBufferT[VERTEXBUFFER_SIZE];
#endif

#if RETRO_PLATFORM == RETRO_PS1
DrawListEntry3D *drawList3D          = s_ps1DummyDraw;
DrawListEntry3D *g_ps1Scene3DScratch = s_ps1DummyScratch; // the draw-list sort's scratch
// The sorted draw list Draw3DScene walks: drawList3D, or the one-pass sort's scratch (no copy back; docs/30 7.2b).
const DrawListEntry3D *g_ps1DrawOrder = s_ps1DummyDraw;

volatile uint32_t g_ps1Scene3DAllocs = 0, g_ps1Scene3DFrees = 0, g_ps1Scene3DAllocFail = 0; // GDB
static void *s_ps1Scene3DBlock = nullptr;
static char s_ps1Scene3DStages[8][24];
static int s_ps1Scene3DStageCount = -1; // -1: Data/Game/PS1Scene3D.bin not read yet
static void PS1Scene3DPoint(uint8_t *b)
{
    if (!b) {
        faceBuffer = s_ps1DummyFace, vertexBuffer = s_ps1DummyVertex, g_ps1Proj = s_ps1DummyProj, g_ps1ExactT = s_ps1DummyExact;
        drawList3D = s_ps1DummyDraw, g_ps1Scene3DScratch = s_ps1DummyScratch;
        g_ps1VtxCap = g_ps1FaceCap = 0;
    }
    else {
        faceBuffer = (Face *)b, b += FACEBUFFER_SIZE * sizeof(Face);
        drawList3D = (DrawListEntry3D *)b, b += FACEBUFFER_SIZE * sizeof(DrawListEntry3D);
        g_ps1Scene3DScratch = (DrawListEntry3D *)b, b += FACEBUFFER_SIZE * sizeof(DrawListEntry3D);
        vertexBuffer = (Vertex *)b, b += VERTEXBUFFER_SIZE * sizeof(Vertex);
        g_ps1Proj = (PS1ProjEntry *)b, b += VERTEXBUFFER_SIZE * sizeof(PS1ProjEntry);
        g_ps1ExactT = (PS1ExactEntry *)b;
        g_ps1VtxCap = VERTEXBUFFER_SIZE, g_ps1FaceCap = FACEBUFFER_SIZE;
    }
    g_ps1DrawOrder = drawList3D;
    vertexCount = faceCount = 0;
}
// Once, first in the boot load (RSDKv4/main.cpp; the file leads the boot's disc order, tools/disc/boot_trace.txt): read
// at a stage load it cost a seek (+20 vblanks on the special stage's load).
void PS1Scene3DReadList()
{
    if (s_ps1Scene3DStageCount >= 0)
        return;
    s_ps1Scene3DStageCount = 0;
    if (FILE *f = fopen("Data/Game/PS1Scene3D.bin", "rb")) {
        uint8_t n = 0;
        if (fread(&n, 1, 1, f) == 1)
            for (int i = 0; i < n && s_ps1Scene3DStageCount < 8; ++i) {
                uint8_t len = 0;
                char name[256];
                if (fread(&len, 1, 1, f) != 1 || fread(name, 1, len, f) != len)
                    break;
                if (len < sizeof(s_ps1Scene3DStages[0])) {
                    memcpy(s_ps1Scene3DStages[s_ps1Scene3DStageCount], name, len);
                    s_ps1Scene3DStages[s_ps1Scene3DStageCount++][len] = 0;
                }
            }
        fclose(f);
    }
}
#define PS1_SCENE3D_ARENA_WORDS (40 * 1024 / 4) // the chain arenas' size while the block is loaded (below)
// First thing in a stage load's bracket (RSDKv4/Scene.cpp), after PS1LoadingBegin freed the chain arenas: the block takes
// the bottom of the free heap and the load's staging buffers, then the arenas, stack above it, as they did above the
// static arrays. Allocated before the bracket, it sat above the arenas' hole and the load spilled past it (special
// stage: 16.7 KB less headroom). The block is zeroed, as the static arrays were at the first load.
void PS1Scene3DPrepare(const char *folder)
{
    PS1Scene3DReadList();
    bool want = false;
    for (int i = 0; i < s_ps1Scene3DStageCount && !want; ++i) want = strcmp(folder, s_ps1Scene3DStages[i]) == 0;
    if (want == (s_ps1Scene3DBlock != nullptr))
        return;
    if (s_ps1Scene3DBlock) {
        psyqo_free(s_ps1Scene3DBlock);
        s_ps1Scene3DBlock = nullptr;
        PS1Scene3DPoint(nullptr);
        PS1ChainSetArenaWords(0); // the normal 64 KB arenas again
        g_ps1Scene3DFrees = g_ps1Scene3DFrees + 1;
        return;
    }
    const size_t size = FACEBUFFER_SIZE * (sizeof(Face) + 2 * sizeof(DrawListEntry3D))
                        + VERTEXBUFFER_SIZE * (sizeof(Vertex) + sizeof(PS1ProjEntry)) + 256 * sizeof(PS1ExactEntry);
    s_ps1Scene3DBlock = psyqo_malloc(size);
    if (!s_ps1Scene3DBlock) {
        g_ps1Scene3DAllocFail = g_ps1Scene3DAllocFail + 1;
        return;
    }
    memset(s_ps1Scene3DBlock, 0, size);
    PS1Scene3DPoint((uint8_t *)s_ps1Scene3DBlock);
    // V4-102: the special stage chains at most 6,028 words a parity (stages 1-7, release EXE, 2026-10-18) of the
    // 16,384 a 64 KB arena holds; its 40 KB arenas (allocated at this load's end, PS1LoadingEnd) leave the heap ~48 KB,
    // which pays for the separate stack (ps1/stack.hh) and lifts the stage's thin headroom (V4-067). Overflow counts
    // in g_ps1ChainOverflow.
    PS1ChainSetArenaWords(PS1_SCENE3D_ARENA_WORDS);
    g_ps1Scene3DAllocs = g_ps1Scene3DAllocs + 1;
}
#else
DrawListEntry3D drawList3D[FACEBUFFER_SIZE];
#endif

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
#if RETRO_PLATFORM == RETRO_PS1 || defined(PS1_INVERSE_UNDER_TEST)
// PS1 (docs/30 phase 7.2b): upstream's MatrixInverse below works in doubles (soft-float here: hundreds of library calls,
// every frame for the special stage's view matrix). For a matrix with last column 0 0 0 and translation 0 (256 in the
// corner: a rotation, as MatrixRotateXYZ makes) all of its products and sums are exact (integers over powers of two)
// until 1.0 / det: integer cofactors, then the same double operations -> the same bits (tools/scene3d/inverse_check.sh,
// 3,000,000 cases). Anything else takes upstream's code.
static bool PS1MatrixInverseRotation(Matrix *matrix)
{
    int(*v)[4] = matrix->values;
    if (v[0][3] || v[1][3] || v[2][3] || v[3][0] || v[3][1] || v[3][2] || v[3][3] != 256)
        return false;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            if (v[y][x] > 32767 || v[y][x] < -32767)
                return false;
    typedef long long i64;
    const i64 a = v[0][0], b = v[0][1], c = v[0][2], d = v[1][0], e = v[1][1], f = v[1][2], g = v[2][0], h = v[2][1], k = v[2][2];
    // 2x2 cofactors (upstream inv[] with this structure's zero terms: exact doubles, over 2^16)
    i64 N[16] = {};
    N[0] = e * k - h * f, N[4] = -d * k + g * f, N[8] = d * h - g * e;
    N[1] = -b * k + h * c, N[5] = a * k - g * c, N[9] = -a * h + g * b;
    N[2] = b * f - e * c, N[6] = -a * f + d * c, N[10] = a * e - d * b;
    const i64 D = a * N[0] + b * N[4] + c * N[8]; // det * 2^24 (upstream's det: exact)
    if (D == 0)
        return true; // upstream returns: matrix unchanged
    const i64 D15 = a * e * k - a * f * h - d * b * k + d * c * h + g * b * f - g * c * e; // inv[15] * 2^24
    double det = 1.0 / ((double)D / 16777216.0);
    int out[16];
    for (int i = 0; i < 16; ++i) {
        if (i != 15 && !N[i]) { // 0.0 * det * 256 truncates to 0 (det is finite): no soft-float calls (docs/43 C12;
            out[i] = 0;         // N[3], [7], [11]-[14] are always 0 here, the others when an entry is)
            continue;
        }
        double inv = i == 15 ? (double)D15 / 16777216.0 : (double)N[i] / 65536.0;
        out[i]     = (int)((inv * det) * 256);
    }
    for (int i = 0; i < 16; ++i) v[i / 4][i % 4] = out[i];
    return true;
}
#endif
void MatrixInverse(Matrix *matrix)
{
#if RETRO_PLATFORM == RETRO_PS1
    if (PS1MatrixInverseRotation(matrix))
        return;
#endif
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
#include "../ps1/gpu_chain.hh"
#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
// Scene3D CPU per call (hblanks, the last call; GDB, docs/30 phase 7): the vertex transform, the depth sort, the faces.
volatile uint32_t g_ps1Prof3DTransform = 0, g_ps1Prof3DSort = 0, g_ps1Prof3DDraw = 0, g_ps1Prof3DVertices = 0, g_ps1Prof3DFaces = 0;
volatile uint32_t g_ps1Prof3DProject = 0; // the GTE projection pass (part of Draw)
volatile uint32_t g_ps1Prof3DFaded = 0, g_ps1Prof3DGeneral = 0, g_ps1Prof3DGeneralN = 0; // Draw: faded runs vs the rest
volatile uint32_t g_ps1Prof3DGenTicks[8], g_ps1Prof3DGenCount[8]; // general path per face flag (root counter 2 ticks, 0.24 us)
#endif
#if RETRO_PLATFORM == RETRO_PS1
// Upstream's transform of vertex v (the per-term floors of TransformVertexBuffer) into its PS1ExactT cache slot
// (Scene3D.hpp; the cache is per TransformVertexBuffer call). Visual only: the scripts never read the transformed buffer.
__attribute__((noinline)) PS1VertexXYZ PS1ExactTOut(int v) { return PS1ExactT(v); }
const PS1VertexXYZ &PS1ExactTSlow(int v)
{
    int vx           = vertexBuffer[v].x;
    int vy           = vertexBuffer[v].y;
    int vz           = vertexBuffer[v].z;
    PS1ExactEntry &e = g_ps1ExactT[v & 255];
    e.x = (vx * matFinal.values[0][0] >> 8) + (vy * matFinal.values[1][0] >> 8) + (vz * matFinal.values[2][0] >> 8) + matFinal.values[3][0];
    e.y = (vx * matFinal.values[0][1] >> 8) + (vy * matFinal.values[1][1] >> 8) + (vz * matFinal.values[2][1] >> 8) + matFinal.values[3][1];
    e.z = (vx * matFinal.values[0][2] >> 8) + (vy * matFinal.values[1][2] >> 8) + (vz * matFinal.values[2][2] >> 8) + matFinal.values[3][2];
    e.idx   = v;
    e.frame = g_ps1ExactFrame;
    return *(const PS1VertexXYZ *)&e;
}
bool PS1Draw3DOpen(); // ps1/render.cpp: a frame is open and objects are shown
extern volatile uint32_t g_ps1Faces;
// Draw3DScene's transform + projection on the GTE (docs/30 phase 7.2b; RSDKv4/Scene3D.cpp TransformVertexBuffer), as
// Sonic CD's special-stage floor: RTPT, three vertices a command (psx-spx "GTE Coordinate Calculation Commands").
// Upstream: T = v * M >> 8 (per term) + translation; screen = SCREEN_CENTER + projectionX * T.x / T.z,
// SCREEN_CENTER - projectionY * T.y / T.z. On the GTE, in units of 32 (the half-pipe's vertices are +-2^18, most of
// them multiples of 32): V = v >> 5, R = M's 3x3 in 4.12 (x 16; the y row scaled by -projectionY / projectionX),
// TR = translation / 32, H = projectionX, OF = the screen centre -> SXY = the screen position (within about a pixel
// of upstream's), SZ = T.z / 32 -> the depth z (.v) for the sort and the fog. Left to the CPU (exact transform + the
// division, marked PS1_PROJ_MARK): a coordinate past +-2^20, a GTE flag (bit 31: IR1 / IR2 / SZ / SX / SY saturated,
// divide overflow; z <= 0 included) or T.z < 8192 (near the camera, where the units of 32 would show); a flagged
// triple is redone one vertex at a time (RTPS) so its good vertices stay on the GTE.
volatile uint32_t g_ps1GteVertices    = 0; // vertices transformed on the GTE (GDB)
volatile uint32_t g_ps1GteFallbacks   = 0; // ... left to the CPU (a flag, near, a range)
volatile uint32_t g_ps1GteLastFlag    = 0;
volatile uint32_t g_ps1GteMatrixCPU   = 0; // frames whose matrix doesn't fit the GTE (all on the CPU)
volatile uint32_t g_ps1GteNear = 0, g_ps1GteFlagged = 0, g_ps1GteRange = 0; // CPU triples by reason (GDB)
static void PS1CpuVertexT(int v)
{
    // only the exact z (the sort's depth, PS1ExactTSlow's formula): x and y come from PS1ExactT if a face drawn by the
    // general path reads them (docs/43 C4: 3 multiplies instead of 9 for the ~85 vertices a frame left to the CPU)
    const Vertex &q = vertexBuffer[v];
    g_ps1Proj[v].u  = 0x8000u;
    g_ps1Proj[v].v  = (q.x * matFinal.values[0][2] >> 8) + (q.y * matFinal.values[1][2] >> 8) + (q.z * matFinal.values[2][2] >> 8) + matFinal.values[3][2];
}
#if PS1_GAME != 1
struct PS1TfState { // RSDKv4/Scene3DTf.S's T_* offsets
    const Vertex *vb;
    PS1ProjEntry *pj;
    int end; // the last whole triple's end
    int ox, oy, oz;
};
static_assert(offsetof(PS1TfState, pj) == 4 && offsetof(PS1TfState, end) == 8 && offsetof(PS1TfState, ox) == 12
                  && offsetof(PS1TfState, oz) == 20 && sizeof(Vertex) == 20,
              "Scene3DTf.S's layout");
extern "C" int PS1TransformAsm(PS1TfState *s, int v);
#endif
void PS1TransformProject3D(int count)
{
    using namespace psyqo::GTE;
    const Matrix &m = matFinal;
    int r[3][3];
    bool fits = true;
    for (int k = 0; k < 3; ++k) {
        r[0][k] = m.values[k][0] * 16;
        r[1][k] = -(m.values[k][1] * 16 * projectionY) / projectionX;
        r[2][k] = m.values[k][2] * 16;
        for (int j = 0; j < 3; ++j) fits = fits && r[j][k] >= -32768 && r[j][k] <= 32767;
    }
    if (!fits) {
        g_ps1GteMatrixCPU = g_ps1GteMatrixCPU + 1;
        for (int v = 0; v < count; ++v) PS1CpuVertexT(v);
        return;
    }
    auto pack = [](int lo, int hi) { return (uint32_t)(uint16_t)lo | ((uint32_t)(uint16_t)hi << 16); };
    write<Register::R11R12, Unsafe>(pack(r[0][0], r[0][1]));
    write<Register::R13R21, Unsafe>(pack(r[0][2], r[1][0]));
    write<Register::R22R23, Unsafe>(pack(r[1][1], r[1][2]));
    write<Register::R31R32, Unsafe>(pack(r[2][0], r[2][1]));
    write<Register::R33, Unsafe>((uint32_t)(uint16_t)r[2][2]);
    // Relative to an origin O (the frame's first vertex): the pipe's world coordinates grow as the stage goes on (past
    // +-2^20 after the third checkpoint, every vertex then left to the CPU: 1,500 hblanks a frame); relative to one of
    // its own vertices they stay small. T = M (v - O) + T(O), T(O) by upstream's formula.
    const int ox = count ? vertexBuffer[0].x : 0, oy = count ? vertexBuffer[0].y : 0, oz = count ? vertexBuffer[0].z : 0;
    const int tx = (ox * m.values[0][0] >> 8) + (oy * m.values[1][0] >> 8) + (oz * m.values[2][0] >> 8) + m.values[3][0];
    const int ty = (ox * m.values[0][1] >> 8) + (oy * m.values[1][1] >> 8) + (oz * m.values[2][1] >> 8) + m.values[3][1];
    const int tz = (ox * m.values[0][2] >> 8) + (oy * m.values[1][2] >> 8) + (oz * m.values[2][2] >> 8) + m.values[3][2];
    write<Register::TRX, Unsafe>((uint32_t)(tx >> 5));
    write<Register::TRY, Unsafe>((uint32_t)(-(ty >> 5) * projectionY / projectionX));
    write<Register::TRZ, Unsafe>((uint32_t)(tz >> 5));
    write<Register::OFX, Unsafe>((uint32_t)(SCREEN_CENTERX << 16));
    write<Register::OFY, Unsafe>((uint32_t)(SCREEN_CENTERY << 16));
    write<Register::H, Safe>((uint32_t)projectionX);
    const uint32_t lim = 1u << 20;
    uint32_t cpu       = 0;
    int v              = 0;
    // the arrays' pointers in locals: the stores below could alias the globals, so GCC reloaded both every triple
    const Vertex *const vb = vertexBuffer;
    PS1ProjEntry *const pj = g_ps1Proj;
#if PS1_GAME != 1
    // the triples the GTE can do in assembly (docs/43 C4: RSDKv4/Scene3DTf.S); it returns at a triple for the code below
    PS1TfState ts;
    ts.vb = vb, ts.pj = pj, ts.end = count - count % 3, ts.ox = ox, ts.oy = oy, ts.oz = oz;
#endif
    for (; v + 3 <= count; v += 3) {
#if PS1_GAME != 1
        v = PS1TransformAsm(&ts, v);
        if (v + 3 > count)
            break;
#endif
        const Vertex *p = &vb[v];
        int dx0 = p[0].x - ox, dy0 = p[0].y - oy, dz0 = p[0].z - oz, dx1 = p[1].x - ox, dy1 = p[1].y - oy, dz1 = p[1].z - oz;
        int dx2 = p[2].x - ox, dy2 = p[2].y - oy, dz2 = p[2].z - oz;
        uint32_t o = (uint32_t)(dx0 + lim) | (uint32_t)(dy0 + lim) | (uint32_t)(dz0 + lim) | (uint32_t)(dx1 + lim)
                     | (uint32_t)(dy1 + lim) | (uint32_t)(dz1 + lim) | (uint32_t)(dx2 + lim) | (uint32_t)(dy2 + lim) | (uint32_t)(dz2 + lim);
        if (o >= 2 * lim) {
            PS1CpuVertexT(v), PS1CpuVertexT(v + 1), PS1CpuVertexT(v + 2);
            cpu += 3;
            g_ps1GteRange = g_ps1GteRange + 1;
            continue;
        }
        write<Register::VXY0, Unsafe>(pack(dx0 >> 5, dy0 >> 5));
        write<Register::VZ0, Unsafe>((uint32_t)(dz0 >> 5));
        write<Register::VXY1, Unsafe>(pack(dx1 >> 5, dy1 >> 5));
        write<Register::VZ1, Unsafe>((uint32_t)(dz1 >> 5));
        write<Register::VXY2, Unsafe>(pack(dx2 >> 5, dy2 >> 5));
        write<Register::VZ2, Safe>((uint32_t)(dz2 >> 5));
        Kernels::rtpt();
        int32_t flag = (int32_t)readRaw<Register::FLAG, Safe>();
        uint32_t z0 = readRaw<Register::SZ1, Safe>(), z1 = readRaw<Register::SZ2, Safe>(), z2 = readRaw<Register::SZ3, Safe>();
        if (flag < 0 || z0 < 256 || z1 < 256 || z2 < 256) { // one vertex at a time (RTPS): only the bad ones go to the CPU
            g_ps1GteLastFlag = flag;
            if (flag < 0)
                g_ps1GteFlagged = g_ps1GteFlagged + 1;
            else
                g_ps1GteNear = g_ps1GteNear + 1;
            for (int k = v; k < v + 3; ++k) {
                const Vertex &q = vertexBuffer[k];
                write<Register::VXY0, Unsafe>(pack((q.x - ox) >> 5, (q.y - oy) >> 5));
                write<Register::VZ0, Safe>((uint32_t)((q.z - oz) >> 5));
                Kernels::rtps();
                int32_t fl = (int32_t)readRaw<Register::FLAG, Safe>();
                uint32_t z = readRaw<Register::SZ3, Safe>();
                if (fl < 0 || z < 256) {
                    PS1CpuVertexT(k);
                    ++cpu;
                    continue;
                }
                g_ps1Proj[k].u = readRaw<Register::SXY2, Safe>();
                g_ps1Proj[k].v = (int)(z << 5);
            }
            continue;
        }
        PS1ProjEntry *t = &pj[v];
        t[0].u    = readRaw<Register::SXY0, Safe>();
        t[1].u    = readRaw<Register::SXY1, Safe>();
        t[2].u    = readRaw<Register::SXY2, Safe>();
        t[0].v    = (int)(z0 << 5);
        t[1].v    = (int)(z1 << 5);
        t[2].v    = (int)(z2 << 5);
    }
    for (; v < count; ++v, ++cpu) PS1CpuVertexT(v);
    g_ps1GteVertices  = g_ps1GteVertices + count;
    g_ps1GteFallbacks = g_ps1GteFallbacks + cpu;
}

// NCLIP side tests (PS1FadedFaces): side(i, j, k) with SXY0-2 = i, j, k; PS1Side2 replaces k only (SXY2).
static inline int PS1Side(uint32_t i, uint32_t j, uint32_t k)
{
    using namespace psyqo::GTE;
    write<Register::SXY0, Unsafe>(i);
    write<Register::SXY1, Unsafe>(j);
    write<Register::SXY2, Safe>(k);
    Kernels::nclip();
    return (int32_t)readRaw<Register::MAC0, Safe>();
}
static inline int PS1Side2(uint32_t k)
{
    using namespace psyqo::GTE;
    write<Register::SXY2, Safe>(k);
    Kernels::nclip();
    return (int32_t)readRaw<Register::MAC0, Safe>();
}
// Draw3DScene's faded faces (the half-pipe: ~650 a frame) in one pass (docs/30 phase 7): from draw-list entry i, every
// following faded face whose four corners the GTE projected (vertexBufferT .u): upstream's fog (the depth's) and
// DrawFadedFace early-outs, the fog blend (cached: runs of faces share colour and fog), the strip order from one or two
// side tests, flat quads written straight into bulk runs of the chain. Returns the entry it stopped at (another kind of
// face, or a corner left to the CPU: Draw3DScene's general path), so the draw order stays the sorted one.
volatile uint32_t g_ps1FadedFast   = 0; // faded faces through the fast path (GDB)
volatile uint32_t g_ps1FadedCulled = 0; // ... of them off screen or degenerate (not drawn)
void PS1FadeRecordRun(uint32_t *first, int count, int stride); // ps1/render.cpp: faded at FlipScreen by a screen fade
int PS1FadedFaces(int i, int count)
{
    const bool draw = PS1Draw3DOpen();
    uint32_t *run   = nullptr;
    int cap = 0, used = 0;
    uint32_t cacheColor = 0, cacheCmd = 0;
    int cacheAlpha = -1;
    uint32_t fast = 0, culled = 0, drawn = 0;
    // the fog colour's 5-bit channels, R and B packed in 16-bit lanes (one multiply blends both: 31 * 255 < 2^16)
    const uint32_t fog = fogColor, fogRB = ((fog >> 19) & 0x1F) | ((fog >> 3) & 0x1F) << 16, fogG = (fog >> 11) & 0x1F;
    const int fogMax = fogStrength, xMax = SCREEN_XSIZE, yMax = SCREEN_YSIZE; // locals: the chain stores can't alias them
    for (; i < count; ++i) {
        const Face &f = faceBuffer[g_ps1DrawOrder[i].faceID];
        if (f.flag != FACE_FLAG_FADED)
            break;
        uint32_t pa = g_ps1Proj[f.a].u, pb = g_ps1Proj[f.b].u, pc = g_ps1Proj[f.c].u, pd = g_ps1Proj[f.d].u;
        // a corner left to the CPU: x = 0x8000; GTE x is -1024..1023, so x + 1024 has bits 11-15 clear
        if (((pa + 1024) | (pb + 1024) | (pc + 1024) | (pd + 1024)) & 0xF800)
            break;
        ++fast;
        int fogStr = (g_ps1DrawOrder[i].depth - 0x8000) >> 8;
        if (fogStr < 0)
            fogStr = 0;
        if (fogStr > fogMax)
            fogStr = fogMax;
        int alpha = 0xFF - fogStr;
        if (!draw || alpha < 1)
            continue;
        if (alpha > 0xFF)
            alpha = 0xFF;
        int x0 = (int16_t)(pa & 0xFFFF), x1 = (int16_t)(pb & 0xFFFF), x2 = (int16_t)(pc & 0xFFFF), x3 = (int16_t)(pd & 0xFFFF);
        int y0 = (int16_t)(pa >> 16), y1 = (int16_t)(pb >> 16), y2 = (int16_t)(pc >> 16), y3 = (int16_t)(pd >> 16);
        if ((x0 < 0 && x1 < 0 && x2 < 0 && x3 < 0) || (x0 > xMax && x1 > xMax && x2 > xMax && x3 > xMax)
            || (y0 < 0 && y1 < 0 && y2 < 0 && y3 < 0) || (y0 > yMax && y1 > yMax && y2 > yMax && y3 > yMax)
            || (x0 == x1 && x1 == x2 && x2 == x3) || (y0 == y1 && y1 == y2 && y2 == y3)) {
            ++culled;
            continue;
        }
        // strip order (PS1QuadStrip): the diagonal is the pair whose line separates the other two; faces listed around
        // their outline (a-c diagonal) are tested first. side(i, j, k) = (xj - xi)(yk - yi) - (yj - yi)(xk - xi) is the
        // GTE's NCLIP of SXY0-2 = i, j, k (psx-spx; GTE positions -1024..1023: no overflow).
        uint32_t w1 = pb, w2 = pa, w3 = pc, w4 = pd; // diagonal a-c: strip b a c d
        int s1 = PS1Side(pa, pc, pb), s3 = PS1Side2(pd);
        if (!((s1 < 0 && s3 > 0) || (s1 > 0 && s3 < 0))) {
            int t0 = PS1Side(pb, pc, pa), t3 = PS1Side2(pd);
            w1 = pa, w2 = pb, w3 = pc, w4 = pd; // diagonal b-c (or collinear): as given
            if (!((t0 < 0 && t3 > 0) || (t0 > 0 && t3 < 0))) {
                int u1 = PS1Side(pa, pd, pb), u2 = PS1Side2(pc);
                if ((u1 < 0 && u2 > 0) || (u1 > 0 && u2 < 0))
                    w1 = pb, w2 = pa, w3 = pd, w4 = pc; // diagonal a-d
            }
        }
        if (f.color != cacheColor || alpha != cacheAlpha) { // upstream's per channel fog5 * (255 - a) >> 8 + c5 * a >> 8
            uint32_t c = f.color, ia = 0xFF - alpha, a = alpha;
            uint32_t cRB = ((c >> 19) & 0x1F) | ((c >> 3) & 0x1F) << 16, cG = (c >> 11) & 0x1F;
            uint32_t rb = ((fogRB * ia) >> 8 & 0x001F001Fu) + ((cRB * a) >> 8 & 0x001F001Fu);
            uint32_t g  = (fogG * ia >> 8) + (cG * a >> 8);
            cacheColor = f.color, cacheAlpha = alpha;
            cacheCmd   = 0x28000000u | rb << 3 | g << 11; // GP0 28h flat quad: R bits 3-7, G 11-15, B 19-23
        }
        if (used == cap) {
            if (run) {
                PS1ChainBulkEnd(used);
                PS1FadeRecordRun(run, used, 6);
            }
            run = PS1ChainBulkBegin(128, 5);
            cap = run ? 128 : 0, used = 0;
            if (!run)
                continue;
        }
        uint32_t *w = run + used * 6;
        w[0]        = cacheCmd;
        w[1]        = w1;
        w[2]        = w2;
        w[3]        = w3;
        w[4]        = w4;
        ++used;
        ++drawn;
    }
    if (run) {
        PS1ChainBulkEnd(used);
        PS1FadeRecordRun(run, used, 6); // 5 words + the chain tag
    }
    g_ps1Faces       = g_ps1Faces + drawn;
    g_ps1FadedFast   = g_ps1FadedFast + fast;
    g_ps1FadedCulled = g_ps1FadedCulled + culled;
    return i;
}

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

#if RETRO_PLATFORM == RETRO_PS1
    ++g_ps1ExactFrame; // a new matFinal: the exact-transform cache starts empty
    PS1TransformProject3D(vertexCount);
    g_ps1Prof3DTransform = PS1HblanksSince(t0);
    g_ps1Prof3DVertices  = vertexCount;
    return;
#else
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
#endif
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
// Stable LSD radix sort, descending by depth (ties keep their order, like upstream's bubble sort): keys hi - depth
// ascending, in as many 9-bit passes as the depth range needs (the half-pipe: 2). One counting loop fills every pass's
// histogram (docs/30 phase 7.2b). Identical order to upstream's: tools/scene3d/sort3d_check.sh.
void PS1SortDrawList3DRange(DrawListEntry3D *list, int count, int lo, int hi)
{
    if (count < 2)
        return;
    unsigned range = (unsigned)hi - (unsigned)lo;
    int passes     = 0;
    while (passes < 4 && (range >> (9 * passes)))
        ++passes;
    if (!passes)
        return;
    uint16_t counts[2][512]; // counts <= FACEBUFFER_SIZE; the first two digits counted in one loop, later ones in theirs
    memset(counts, 0, sizeof(counts));
    for (int i = 0; i < count; ++i) {
        unsigned k = (unsigned)hi - (unsigned)list[i].depth;
        ++counts[0][k & 511];
        ++counts[1][(k >> 9) & 511];
    }
    DrawListEntry3D *src = list, *dst = g_ps1Scene3DScratch;
    for (int p = 0; p < passes; ++p) {
        uint16_t *c = counts[p & 1];
        int shift   = 9 * p;
        if (p >= 2) {
            memset(c, 0, sizeof(counts[0]));
            for (int i = 0; i < count; ++i) ++c[(((unsigned)hi - (unsigned)src[i].depth) >> shift) & 511];
        }
        int at = 0;
        for (int b = 0; b < 512; ++b) {
            int n = c[b];
            c[b]  = at;
            at += n;
        }
        for (int i = 0; i < count; ++i) dst[c[(((unsigned)hi - (unsigned)src[i].depth) >> shift) & 511]++] = src[i];
        DrawListEntry3D *t = src;
        src                = dst;
        dst                = t;
    }
    if (src != list)
        for (int i = 0; i < count; ++i) list[i] = src[i];
}
#if RETRO_PLATFORM == RETRO_PS1
// The special stage's draw-list sort (docs/30 phase 7.2b, a night choice like the GTE transform: visual only): one
// stable counting pass on the top 10 bits of the depth range (1,024 buckets; the half-pipe's ~2^18 range -> 256 depth
// units a bucket). Faces in the same bucket keep their list order (the pipe's in segment order, then the rings and the
// player, which the scripts add after it), so only faces within one bucket's depth of each other can differ from
// upstream's exact order. g_ps1Sort3DExact = 1 (GDB) restores the exact two-pass sort.
volatile uint32_t g_ps1Sort3DExact = 0;
void PS1SortDrawList3DCoarse(DrawListEntry3D *list, int count, int lo, int hi)
{
    if (count < 2)
        return;
    unsigned range = (unsigned)hi - (unsigned)lo;
    int shift      = 0;
    while ((range >> shift) >= 1024) ++shift;
    uint16_t counts[1024];
    memset(counts, 0, sizeof(counts));
    for (int i = 0; i < count; ++i) ++counts[((unsigned)hi - (unsigned)list[i].depth) >> shift];
    int at = 0;
    for (int b = 0; b < 1024; ++b) {
        int n     = counts[b];
        counts[b] = at;
        at += n;
    }
    DrawListEntry3D *dst = g_ps1Scene3DScratch;
    for (int i = 0; i < count; ++i) dst[counts[((unsigned)hi - (unsigned)list[i].depth) >> shift]++] = list[i];
    g_ps1DrawOrder = dst; // Draw3DScene reads the scratch (no copy back)
}
#endif
void PS1SortDrawList3D(DrawListEntry3D *list, int count)
{
    if (count < 2)
        return;
    int lo = list[0].depth, hi = list[0].depth;
    for (int i = 1; i < count; ++i) {
        lo = list[i].depth < lo ? list[i].depth : lo;
        hi = list[i].depth > hi ? list[i].depth : hi;
    }
    PS1SortDrawList3DRange(list, count, lo, hi);
}
#endif

void Sort3DDrawList()
{
#if RETRO_PLATFORM == RETRO_PS1
    uint32_t t0 = PS1Hblanks();
#endif
#if RETRO_PLATFORM == RETRO_PS1
    int lo = 0, hi = 0;
    for (int i = 0; i < faceCount; ++i) { // .v: the depth z (the GTE's, or the exact one for marked vertices)
        const Face &f = faceBuffer[i];
        int depth     = (g_ps1Proj[f.d].v + g_ps1Proj[f.c].v + g_ps1Proj[f.b].v + g_ps1Proj[f.a].v) >> 2;
        drawList3D[i].depth  = depth;
        drawList3D[i].faceID = i;
        if (!i)
            lo = hi = depth;
        lo = depth < lo ? depth : lo;
        hi = depth > hi ? depth : hi;
    }
#else
    for (int i = 0; i < faceCount; ++i) {
        drawList3D[i].depth = (vertexBufferT[faceBuffer[i].d].z + vertexBufferT[faceBuffer[i].c].z + vertexBufferT[faceBuffer[i].b].z
                               + vertexBufferT[faceBuffer[i].a].z)
                              >> 2;
        drawList3D[i].faceID = i;
    }
#endif

#if RETRO_PLATFORM == RETRO_PS1
    // PS1: the bubble sort below is stable and descending, so any stable descending sort gives the same order: a radix
    // sort (docs/30 phase 7; the half-pipe's 742 faces took 4,266 hblanks bubbled, 220 merge-sorted). Identical order
    // checked on the host: tools/scene3d/sort3d_check.sh.
    g_ps1DrawOrder = drawList3D; // the exact sort (and a list under 2 faces) stays in place
    if (g_ps1Sort3DExact)
        PS1SortDrawList3DRange(drawList3D, faceCount, lo, hi);
    else
        PS1SortDrawList3DCoarse(drawList3D, faceCount, lo, hi);
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
// vertexBufferT[i].u: the GTE's screen position (upstream's SCREEN_CENTER +- projection * T / T.z within about a pixel);
// a vertex the GTE can't do (near the camera, off the GTE's screen range) is marked and projected here exactly (its exact
// T is computed: PS1VertexT before the face is drawn).
#define PS1_PROJ_MARK (0x8000u)
static inline void PS1ProjectedVertex(int i, int *x, int *y)
{
    uint32_t p = g_ps1Proj[i].u;
    if ((p & 0xFFFF) != PS1_PROJ_MARK) {
        *x = (int16_t)(p & 0xFFFF);
        *y = (int16_t)(p >> 16);
    }
    else {
        PS1VertexXYZ t = PS1ExactT(i);
        *x = SCREEN_CENTERX + projectionX * t.x / t.z;
        *y = SCREEN_CENTERY - projectionY * t.y / t.z;
    }
}
int PS1FadedFaces(int i, int count); // above: a run of faded faces, returns where it stopped
void PS1TexturedRect(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, byte sheetID, bool blended); // ps1/render.cpp
bool PS1TexturedRectWords(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, byte sheetID, bool blended, uint32_t *w);
#endif
#if RETRO_PLATFORM == RETRO_PS1
static uint32_t s_ps1FadedHbl = 0, s_ps1GeneralN = 0; // this Draw3DScene: faded runs' hblanks, general-path faces
static void PS1Draw3DList(int spriteSheetID, int count);
void Draw3DScene(int spriteSheetID)
{
    uint32_t t0        = PS1Hblanks();
    g_ps1Prof3DProject = 0; // the projection is part of the transform now (PS1TransformProject3D)
    s_ps1FadedHbl = s_ps1GeneralN = 0;
    for (int k = 0; k < 8; ++k) g_ps1Prof3DGenTicks[k] = g_ps1Prof3DGenCount[k] = 0;
    PS1Draw3DList(spriteSheetID, faceCount);
    g_ps1Prof3DDraw     = PS1HblanksSince(t0);
    g_ps1Prof3DFaded    = s_ps1FadedHbl;
    g_ps1Prof3DGeneral  = g_ps1Prof3DDraw - s_ps1FadedHbl;
    g_ps1Prof3DGeneralN = s_ps1GeneralN;
}
// Draw3DScene's loop over the first `count` entries of g_ps1DrawOrder (PS1Draw3DSceneOT runs it on one entry at a time)
static void PS1Draw3DList(int spriteSheetID, int count)
#if PS1_GAME != 1
    ;
// Sort3DDrawList + Draw3DScene as an ordering table (docs/30 "Mania lessons pass", 2026-10-09; Driver 2's model, docs/38
// 2.3, as Mania's UFO stages, docs/32 9f): one pass over the faces in list order computes each face's depth (the sort's
// formula) and its primitives -- the faded fast path writes its flat quad into the arena unlinked, any other face is
// drawn by Draw3DScene's own code on a one-entry draw order while the chain captures what it emits -- and a second pass
// links every face, from the last to the first, as the new head of bucket (hi - depth) >> shift: the coarse sort's key,
// with its shift from the same depth range. The GPU walks the buckets from 0 (the farthest) and each bucket in list
// order: the order PS1SortDrawList3DCoarse gives, without its counting, prefix and scatter passes or the draw order's
// indirection. g_ps1Draw3DOT = 0 (GDB) draws through the sort again (the same-build A/B: identical pictures).
volatile uint32_t g_ps1Draw3DOT      = 1;
volatile uint32_t g_ps1OTBuckets     = 0; // last frame: buckets in the table
volatile uint32_t g_ps1OTCaptured    = 0; // last frame: faces drawn by the general path (captured sub-lists)
volatile uint32_t g_ps1OTFailed      = 0; // frames whose table didn't fit the arena (total; primitives dropped)
volatile uint32_t g_ps1Draw3DTexFast = 1; // GDB: 0 = TEXTURED_C / _BLEND faces through the general path (same-build A/B)
volatile uint32_t g_ps1OTTexFast     = 0; // last frame: textured quads written directly (docs/43 C1)
struct PS1OTRec {
    int depth;
    uint32_t *first; // the face's first node (bit 0: a captured sub-list, its last node in g_ps1Scene3DScratch), or null
};
static_assert(sizeof(PS1OTRec) == sizeof(DrawListEntry3D), "PS1OTRec reuses drawList3D");
// The ordering table's faces other than the faded fast path (docs/43 C1): out of line, so the hot loop below keeps its
// state in registers (with these paths inline GCC spilled ~30 values to the stack). Returns the face's record pointer
// (tagged as in the link pass) or null. The caller has closed any open bulk run.
static uint32_t s_ps1OTCaptured = 0, s_ps1OTTex = 0, s_ps1OTFadedGen[3];
volatile uint32_t g_ps1Draw3DFadedFast = 1; // GDB: 0 = faded faces with a CPU corner through the captured general path
volatile uint32_t g_ps1OTFadedGen[3];       // last frame: those faces not drawn / written as words / beyond the limits
int PS1FadedFaceWords(const Vertex *q, uint color, uint fogColor, int alpha, uint32_t *w); // ps1/render.cpp
static __attribute__((noinline)) uint32_t *PS1OTOtherFace(int i, int depth, int spriteSheetID, bool draw)
{
    const Face &f = faceBuffer[i];
    // the rings', bombs' and shadows' textured quads (TEXTURED_C / _BLEND: ~30-55 a frame): the general path's code
    // (PS1Draw3DList's two cases) with the 9 words written straight into the arena, unlinked like a faded quad
    if ((f.flag == FACE_FLAG_TEXTURED_C || f.flag == FACE_FLAG_TEXTURED_C_BLEND) && g_ps1Draw3DTexFast) {
        if (!draw)
            return nullptr;
        PS1VertexXYZ ta = PS1ExactT(f.a);
        if (ta.z <= 0)
            return nullptr;
        const Vertex &vb = vertexBuffer[f.b], &va = vertexBuffer[f.a], &vc = vertexBuffer[f.c];
        uint32_t w9[9];
        if (!PS1TexturedRectWords(SCREEN_CENTERX + projectionX * (ta.x - vb.u) / ta.z, SCREEN_CENTERY - projectionY * (ta.y + vb.v) / ta.z,
                                  SCREEN_CENTERX + projectionX * (ta.x + vb.u) / ta.z, SCREEN_CENTERY - projectionY * (ta.y - vb.v) / ta.z,
                                  va.u - vc.u, va.v - vc.v, va.u + vc.u, va.v + vc.v, spriteSheetID, f.flag == FACE_FLAG_TEXTURED_C_BLEND, w9))
            return nullptr;
        uint32_t *h = PS1ChainOTRaw(9);
        if (!h)
            return nullptr;
        for (int k = 0; k < 9; ++k) h[1 + k] = w9[k];
        ++s_ps1OTTex;
        g_ps1Faces = g_ps1Faces + 1;
        return reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(h) | 2);
    }
    // a faded face with a corner left to the CPU (near the camera or beyond the GTE's screen range): upstream's general
    // case (PS1Draw3DList: exact corners, fog, DrawFadedFace) as words when the quad fits the GPU limits (docs/43 C1)
    if (f.flag == FACE_FLAG_FADED && g_ps1Draw3DFadedFast) {
        if (g_ps1Proj[f.a].v <= 0 || g_ps1Proj[f.b].v <= 0 || g_ps1Proj[f.c].v <= 0 || g_ps1Proj[f.d].v <= 0) // PS1_ZPOS
            return nullptr;
        Vertex quad[4];
        PS1ProjectedVertex(f.a, &quad[0].x, &quad[0].y);
        PS1ProjectedVertex(f.b, &quad[1].x, &quad[1].y);
        PS1ProjectedVertex(f.c, &quad[2].x, &quad[2].y);
        PS1ProjectedVertex(f.d, &quad[3].x, &quad[3].y);
        int fogStr = 0;
        if ((depth - 0x8000) >> 8 >= 0)
            fogStr = (depth - 0x8000) >> 8;
        if (fogStr > fogStrength)
            fogStr = fogStrength;
        uint32_t w5[5];
        int r = PS1FadedFaceWords(quad, f.color, fogColor, 0xFF - fogStr, w5);
        if (r == 0) {
            ++s_ps1OTFadedGen[0];
            return nullptr;
        }
        if (r == 1) {
            ++s_ps1OTFadedGen[1];
            uint32_t *h = PS1ChainOTRaw(5);
            if (!h)
                return nullptr;
            for (int k = 0; k < 5; ++k) h[1 + k] = w5[k];
            g_ps1Faces = g_ps1Faces + 1;
            return h; // a faded quad: the link pass writes its 5-word header
        }
        ++s_ps1OTFadedGen[2]; // beyond the limits: the clipped polygon, through the general path below
    }
    // any other face: Draw3DScene's general path on a one-entry draw order, captured
    DrawListEntry3D one;
    one.faceID     = i;
    one.depth      = depth;
    g_ps1DrawOrder = &one;
    PS1ChainCaptureBegin();
    PS1Draw3DList(spriteSheetID, 1);
    uint32_t *first, *last, size;
    if (!PS1ChainCaptureEnd(&first, &last, &size))
        return nullptr;
    DrawListEntry3D *subLast = g_ps1Scene3DScratch; // a captured face: .faceID = its last node, .depth = that node's size word
    subLast[i].faceID        = (int)reinterpret_cast<uintptr_t>(last);
    subLast[i].depth         = (int)size;
    ++s_ps1OTCaptured;
    return reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(first) | 1);
}
// The faded fast path of the pass below, per face, in assembly (docs/43 C1: RSDKv4/Scene3DOT.S; GCC's loop spilled ~10
// values a face). The asm reads and writes this state; offsets are its S_* constants.
struct PS1OTFastState {
    const Face *fb;
    PS1OTRec *rec;
    const PS1ProjEntry *pj;
    int count;
    int lo, hi;
    uint32_t cacheColor;
    int cacheAlpha;
    uint32_t cacheCmd;
    uint32_t *w, *wEnd; // the open bulk run: next primitive, end of its capacity (both null: no run open)
    int fogMax;
    int skipAt; // draw ? 255 : INT_MIN: skipped when the clamped fog >= this (!draw || alpha < 1)
    int qxn, qyn; // -(SCREEN_XSIZE + 1), -((SCREEN_YSIZE + 1) << 16)
    uint32_t fogRB, fogG;
    uint32_t culled;
    int pending;    // returned with a face ready to write but no room: its five words in pw
    uint32_t pw[5];
    uint32_t c2Color, c2AlphaRGB; // the asm's second colour-cache entry: colour, alpha << 24 | command bits 0-23
};
static_assert(offsetof(PS1OTFastState, fb) == 0 && offsetof(PS1OTFastState, rec) == 4 && offsetof(PS1OTFastState, pj) == 8
                  && offsetof(PS1OTFastState, count) == 12 && offsetof(PS1OTFastState, lo) == 16 && offsetof(PS1OTFastState, hi) == 20
                  && offsetof(PS1OTFastState, cacheColor) == 24 && offsetof(PS1OTFastState, cacheAlpha) == 28
                  && offsetof(PS1OTFastState, cacheCmd) == 32 && offsetof(PS1OTFastState, w) == 36 && offsetof(PS1OTFastState, wEnd) == 40
                  && offsetof(PS1OTFastState, fogMax) == 44 && offsetof(PS1OTFastState, skipAt) == 48 && offsetof(PS1OTFastState, qxn) == 52
                  && offsetof(PS1OTFastState, qyn) == 56 && offsetof(PS1OTFastState, fogRB) == 60 && offsetof(PS1OTFastState, fogG) == 64
                  && offsetof(PS1OTFastState, culled) == 68 && offsetof(PS1OTFastState, pending) == 72 && offsetof(PS1OTFastState, pw) == 76
                  && offsetof(PS1OTFastState, c2Color) == 96 && offsetof(PS1OTFastState, c2AlphaRGB) == 100,
              "PS1OTFastState: RSDKv4/Scene3DOT.S's S_* offsets");
static_assert(sizeof(Face) == 24 && offsetof(Face, color) == 16 && offsetof(Face, flag) == 20 && sizeof(PS1ProjEntry) == 8
                  && sizeof(PS1OTRec) == 8 && FACE_FLAG_FADED == 4,
              "Scene3DOT.S's face / projection / record layout");
extern "C" int PS1OTFadedAsm(PS1OTFastState *s, int i);
volatile uint32_t g_ps1OTAsmCalls = 0; // last frame: entries into the asm fast path (GDB)
#if PS1_OT_CHECK
// make OT_CHECK=1 (harness only; make dist refuses it): after every asm call, the C fast path as it was before the asm
// (2d87dee's loop) replays the same faces from the state the asm started from and compares what the asm returned and
// wrote: records, primitive words, lo / hi, the colour cache, w, culled, the pending face. Counters for GDB / the suite.
volatile uint32_t g_ps1OTCheckFaces = 0, g_ps1OTCheckMismatch = 0, g_ps1OTCheckFirstFace = 0xFFFFFFFFu, g_ps1OTCheckFirstFrame = 0;
// coverage: 0 drawn, 1 skipped (alpha / !draw), 2 culled all x < 0, 3 x > max, 4 y < 0, 5 y > max, 6 x equal, 7 y equal,
// 8 order b a c d, 9 a b c d (second test), 10 b a d c, 11 a b c d (no test), 12 cache miss, 13 pending, 14 fogStr < 0
// before the clamp, 15 alpha clamped to 255 (fogMax < 0), 16 other face (returned), 17 !draw
volatile uint32_t g_ps1OTCheckCover[18];
// GDB injection for the branches the stages never reach (check build only): a fogStrength override (alpha < 1 skips at
// >= 255, the alpha clamp below 0) and a forced !draw
volatile int g_ps1OTCheckFogMax = 0x7FFFFFFF, g_ps1OTCheckNoDraw = 0;
extern volatile uint32_t g_ps1PerfFrames;
static void PS1OTCheckFail(int i)
{
    g_ps1OTCheckMismatch = g_ps1OTCheckMismatch + 1;
    if (g_ps1OTCheckFirstFace == 0xFFFFFFFFu) {
        g_ps1OTCheckFirstFace  = i;
        g_ps1OTCheckFirstFrame = g_ps1PerfFrames;
    }
}
static void PS1OTCheck(const PS1OTFastState &s0, int i, const PS1OTFastState &out, int ret, bool draw)
{
    PS1OTFastState s = s0;
    const Face *const fb = s.fb;
    const PS1ProjEntry *const pj = s.pj;
    const uint32_t qx = -s.qxn, qy = -s.qyn;
    if (!draw)
        g_ps1OTCheckCover[17] = g_ps1OTCheckCover[17] + 1;
    for (; i < s.count; ++i) {
        const Face &f = fb[i];
        const PS1ProjEntry &A = pj[f.a], &B = pj[f.b], &C = pj[f.c], &D = pj[f.d];
        const int depth = (D.v + C.v + B.v + A.v) >> 2;
        uint32_t pa = A.u, pb = B.u, pc = C.u, pd = D.u;
        if (f.flag != FACE_FLAG_FADED || (((pa + 1024) | (pb + 1024) | (pc + 1024) | (pd + 1024)) & 0xF800)) {
            g_ps1OTCheckCover[16] = g_ps1OTCheckCover[16] + 1;
            break;
        }
        g_ps1OTCheckFaces = g_ps1OTCheckFaces + 1;
        s.lo = depth < s.lo ? depth : s.lo;
        s.hi = depth > s.hi ? depth : s.hi;
        if (s.rec[i].depth != depth)
            PS1OTCheckFail(i);
        int fogStr = (depth - 0x8000) >> 8;
        if (fogStr < 0)
            fogStr = 0, g_ps1OTCheckCover[14] = g_ps1OTCheckCover[14] + 1;
        if (fogStr > s.fogMax)
            fogStr = s.fogMax;
        int alpha = 0xFF - fogStr;
        if (!draw || alpha < 1) {
            g_ps1OTCheckCover[1] = g_ps1OTCheckCover[1] + 1;
            if (s.rec[i].first)
                PS1OTCheckFail(i);
            continue;
        }
        if (alpha > 0xFF)
            alpha = 0xFF, g_ps1OTCheckCover[15] = g_ps1OTCheckCover[15] + 1;
        int x0 = (int16_t)(pa & 0xFFFF), x1 = (int16_t)(pb & 0xFFFF), x2 = (int16_t)(pc & 0xFFFF), x3 = (int16_t)(pd & 0xFFFF);
        int y0 = (int16_t)(pa >> 16), y1 = (int16_t)(pb >> 16), y2 = (int16_t)(pc >> 16), y3 = (int16_t)(pd >> 16);
        const int xMax = (int)qx - 1, yMax = (int)(qy >> 16) - 1;
        int cull = (x0 < 0 && x1 < 0 && x2 < 0 && x3 < 0) ? 2 : (x0 > xMax && x1 > xMax && x2 > xMax && x3 > xMax) ? 3
                 : (y0 < 0 && y1 < 0 && y2 < 0 && y3 < 0) ? 4 : (y0 > yMax && y1 > yMax && y2 > yMax && y3 > yMax) ? 5
                 : (x0 == x1 && x1 == x2 && x2 == x3) ? 6 : (y0 == y1 && y1 == y2 && y2 == y3) ? 7 : 0;
        // 2d87dee's packed tests decide, the unpacked ones only label the coverage
        const uint32_t pn = pa & pb & pc & pd;
        const uint32_t px = (pa - qx) | (pb - qx) | (pc - qx) | (pd - qx), py = (pa - qy) | (pb - qy) | (pc - qy) | (pd - qy);
        const uint32_t eq = (pa ^ pb) | (pa ^ pc) | (pa ^ pd);
        if ((pn & 0x80008000u) || !(px & 0x8000) || !(py & 0x80000000u) || !(eq & 0xFFFF) || !(eq & 0xFFFF0000u)) {
            ++s.culled;
            g_ps1OTCheckCover[cull] = g_ps1OTCheckCover[cull] + 1;
            if (s.rec[i].first)
                PS1OTCheckFail(i);
            continue;
        }
        uint32_t w1 = pb, w2 = pa, w3 = pc, w4 = pd;
        int path = 8;
        int s1 = PS1Side(pa, pc, pb), s3 = PS1Side2(pd);
        if (!((s1 < 0 && s3 > 0) || (s1 > 0 && s3 < 0))) {
            int t0s = PS1Side(pb, pc, pa), t3 = PS1Side2(pd);
            w1 = pa, w2 = pb, w3 = pc, w4 = pd, path = 9;
            if (!((t0s < 0 && t3 > 0) || (t0s > 0 && t3 < 0))) {
                int u1 = PS1Side(pa, pd, pb), u2 = PS1Side2(pc);
                path = 11;
                if ((u1 < 0 && u2 > 0) || (u1 > 0 && u2 < 0))
                    w1 = pb, w2 = pa, w3 = pd, w4 = pc, path = 10;
            }
        }
        g_ps1OTCheckCover[path] = g_ps1OTCheckCover[path] + 1;
        if (f.color != s.cacheColor || alpha != s.cacheAlpha) {
            uint32_t c = f.color, ia = 0xFF - alpha, a = alpha;
            uint32_t cRB = ((c >> 19) & 0x1F) | ((c >> 3) & 0x1F) << 16, cG = (c >> 11) & 0x1F;
            uint32_t rb = ((s.fogRB * ia) >> 8 & 0x001F001Fu) + ((cRB * a) >> 8 & 0x001F001Fu);
            uint32_t g  = (s.fogG * ia >> 8) + (cG * a >> 8);
            s.cacheColor = f.color, s.cacheAlpha = alpha;
            s.cacheCmd   = 0x28000000u | rb << 3 | g << 11;
            g_ps1OTCheckCover[12] = g_ps1OTCheckCover[12] + 1;
        }
        if (s.w == s.wEnd) { // the asm must stop here with this face pending
            g_ps1OTCheckCover[13] = g_ps1OTCheckCover[13] + 1;
            if (ret != i || !out.pending || out.pw[0] != s.cacheCmd || out.pw[1] != w1 || out.pw[2] != w2 || out.pw[3] != w3
                || out.pw[4] != w4 || s.rec[i].first)
                PS1OTCheckFail(i);
            s.pending = 1;
            break;
        }
        g_ps1OTCheckCover[0] = g_ps1OTCheckCover[0] + 1;
        uint32_t *w = s.w;
        if (w[0] != s.cacheCmd || w[1] != w1 || w[2] != w2 || w[3] != w3 || w[4] != w4 || s.rec[i].first != w - 1)
            PS1OTCheckFail(i);
        s.w = w + 6;
    }
    // (the asm's colour cache has two entries, the C loop's one: their contents differ, the words they give don't)
    if (ret != i || (bool)out.pending != (bool)s.pending || out.lo != s.lo || out.hi != s.hi || out.w != s.w || out.culled != s.culled)
        PS1OTCheckFail(i);
}
#endif
void PS1Draw3DSceneOT(int spriteSheetID)
{
    uint32_t t0        = PS1Hblanks();
    g_ps1Prof3DProject = 0;
    s_ps1FadedHbl = s_ps1GeneralN = 0;
    s_ps1OTCaptured = s_ps1OTTex = 0;
    s_ps1OTFadedGen[0] = s_ps1OTFadedGen[1] = s_ps1OTFadedGen[2] = 0;
    for (int k = 0; k < 8; ++k) g_ps1Prof3DGenTicks[k] = g_ps1Prof3DGenCount[k] = 0;
    const int count = faceCount;
    PS1OTRec *rec   = reinterpret_cast<PS1OTRec *>(drawList3D);
#if PS1_OT_CHECK
    const bool draw = PS1Draw3DOpen() && !g_ps1OTCheckNoDraw;
#else
    const bool draw = PS1Draw3DOpen();
#endif
    uint32_t *run   = nullptr; // the open bulk run (its next primitive and end: st.w, st.wEnd)
    uint32_t fast = 0, drawn = 0, calls = 0;
    const uint32_t fog = fogColor;
    PS1OTFastState st;
    st.fb = faceBuffer; // the record / chain stores could alias the globals: the asm keeps its own copies
    st.rec = rec;
    st.pj  = g_ps1Proj;
    st.count = count;
    st.lo = 0x7FFFFFFF, st.hi = (int)0x80000000; // the first face sets both (the C loop's `if (!i) lo = hi = depth`)
    st.cacheColor = 0, st.cacheAlpha = -1, st.cacheCmd = 0;
    st.c2Color = 0, st.c2AlphaRGB = 0; // alpha 0: empty (a drawn face's alpha is 1..255)
    st.w = st.wEnd = nullptr;
    st.fogMax = fogStrength;
#if PS1_OT_CHECK
    if (g_ps1OTCheckFogMax != 0x7FFFFFFF)
        st.fogMax = g_ps1OTCheckFogMax;
#endif
    st.skipAt = draw ? 0xFF : (int)0x80000000;
    st.qxn    = -(SCREEN_XSIZE + 1);
    st.qyn    = -((SCREEN_YSIZE + 1) << 16);
    st.fogRB  = ((fog >> 19) & 0x1F) | ((fog >> 3) & 0x1F) << 16, st.fogG = (fog >> 11) & 0x1F;
    st.culled = 0, st.pending = 0;
    const Face *const fb        = faceBuffer;
    const PS1ProjEntry *const pj = g_ps1Proj;
    auto closeRun = [&]() {
        if (run) {
            int used = (int)((st.w - run) / 6);
            PS1ChainBulkKeep(used);
            PS1FadeRecordRun(run, used, 6);
            drawn += used;
            run = nullptr;
        }
        st.w = st.wEnd = nullptr; // a new run opens at the next faded face (the arena after the closed run is the others')
    };
    for (int i = 0; i < count;) {
        const int i0 = i;
#if PS1_OT_CHECK
        const PS1OTFastState before = st;
#endif
        i = PS1OTFadedAsm(&st, i);
        ++calls;
#if PS1_OT_CHECK
        PS1OTCheck(before, i0, st, i, draw);
#endif
        fast += i - i0;
        if (st.pending) { // a faded face with no room: open a run (as the C loop did at w == wEnd) and write it
            st.pending = 0;
            ++fast;
            closeRun();
            run = PS1ChainBulkBegin(128, 5);
            if (run) {
                uint32_t *w = run;
                st.wEnd     = run + 128 * 6;
                w[0]        = st.pw[0];
                w[1]        = st.pw[1];
                w[2]        = st.pw[2];
                w[3]        = st.pw[3];
                w[4]        = st.pw[4];
                rec[i].first = w - 1; // its DMA header, written by the second pass
                st.w         = w + 6;
            }
            ++i;
            continue;
        }
        if (i >= count)
            break;
        // any other face: its depth and record (the asm returned before touching it), then the general paths
        const Face &f = fb[i];
        const PS1ProjEntry &A = pj[f.a], &B = pj[f.b], &C = pj[f.c], &D = pj[f.d];
        const int depth = (D.v + C.v + B.v + A.v) >> 2; // Sort3DDrawList's
        st.lo         = depth < st.lo ? depth : st.lo;
        st.hi         = depth > st.hi ? depth : st.hi;
        rec[i].depth  = depth;
        closeRun(); // what the other paths allocate goes after the run in the arena
        rec[i].first = PS1OTOtherFace(i, depth, spriteSheetID, draw);
        ++i;
    }
    closeRun();
    g_ps1OTAsmCalls = calls;
    const uint32_t culled = st.culled;
    const int lo = st.lo, hi = st.hi;
    g_ps1DrawOrder = drawList3D;
    uint32_t t1    = PS1Hblanks();
    const DrawListEntry3D *subLast = g_ps1Scene3DScratch;
    if (count) {
        unsigned range = (unsigned)hi - (unsigned)lo;
        int shift      = 0;
        while ((range >> shift) >= 1024) ++shift;
        const int buckets = (int)(range >> shift) + 1;
        uint32_t *ot      = PS1ChainOTBegin(buckets);
        g_ps1OTBuckets    = buckets;
        if (!ot)
            g_ps1OTFailed = g_ps1OTFailed + 1;
        else
            for (int i = count - 1; i >= 0; --i) {
                uint32_t *p = rec[i].first;
                if (!p)
                    continue;
                uint32_t *slot = &ot[((unsigned)hi - (unsigned)rec[i].depth) >> shift];
                uint32_t link  = *slot; // an empty tag's size is 0: the word is the next node's address
                const uintptr_t tag = reinterpret_cast<uintptr_t>(p) & 3;
                if (tag == 1) {
                    p = reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(p) & ~(uintptr_t)1);
                    *reinterpret_cast<uint32_t *>(subLast[i].faceID) = (uint32_t)subLast[i].depth | link;
                }
                else if (tag == 2) { // a textured quad: 9 words
                    p  = reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(p) & ~(uintptr_t)3);
                    *p = (9u << 24) | link;
                }
                else
                    *p = (5u << 24) | link; // a faded quad: 5 words
                *slot = reinterpret_cast<uintptr_t>(p) & 0xffffff;
            }
    }
    g_ps1Faces          = g_ps1Faces + drawn;
    g_ps1FadedFast      = g_ps1FadedFast + fast;
    g_ps1FadedCulled    = g_ps1FadedCulled + culled;
    g_ps1OTCaptured     = s_ps1OTCaptured;
    g_ps1OTTexFast      = s_ps1OTTex;
    for (int k = 0; k < 3; ++k) g_ps1OTFadedGen[k] = s_ps1OTFadedGen[k];
    g_ps1Prof3DSort     = PS1HblanksSince(t1); // the second pass (linking)
    g_ps1Prof3DFaces    = count;
    g_ps1Prof3DDraw     = PS1HblanksSince(t0);
    g_ps1Prof3DFaded    = s_ps1FadedHbl;
    g_ps1Prof3DGeneral  = g_ps1Prof3DDraw - s_ps1FadedHbl;
    g_ps1Prof3DGeneralN = s_ps1GeneralN;
}
static void PS1Draw3DList(int spriteSheetID, int count)
#endif
#else
void Draw3DScene(int spriteSheetID)
#endif
{
#if RETRO_PLATFORM == RETRO_PS1
// upstream's T.z > 0 without the exact transform (docs/30 "Mania lessons pass"): g_ps1Proj[v].v is the exact z for a
// vertex left to the CPU (PS1CpuVertexT), and SZ * 32 >= 8192 for one the GTE projected, whose exact z is within ~840 of
// it (units of 32, a 4.12 matrix of entries < 8: the truncations' worst case) and so > 0 as well: the same decision.
#define PS1_ZPOS(idx) (g_ps1Proj[idx].v > 0)
#define PS1_PROJECT_QUAD()                                                                                                                  \
    PS1ProjectedVertex(face->a, &quad[0].x, &quad[0].y);                                                                                  \
    PS1ProjectedVertex(face->b, &quad[1].x, &quad[1].y);                                                                                  \
    PS1ProjectedVertex(face->c, &quad[2].x, &quad[2].y);                                                                                  \
    PS1ProjectedVertex(face->d, &quad[3].x, &quad[3].y);
#endif
    Vertex quad[4];
#if RETRO_PLATFORM == RETRO_PS1
    for (int i = 0; i < count; ++i) {
#else
    for (int i = 0; i < faceCount; ++i) {
#endif
#if RETRO_PLATFORM == RETRO_PS1
        Face *face = &faceBuffer[g_ps1DrawOrder[i].faceID];
#else
        Face *face = &faceBuffer[drawList3D[i].faceID];
#endif
#if RETRO_PLATFORM == RETRO_PS1
        if (face->flag == FACE_FLAG_FADED) { // the fast path: the run of faded faces whose corners the GTE projected
            uint32_t tf = PS1Hblanks();
            int next    = PS1FadedFaces(i, count);
            s_ps1FadedHbl += PS1HblanksSince(tf);
            if (next != i) {
                i = next - 1;
                continue;
            }
        }
        // the general path: upstream's code; its vertexBufferT reads get the exact transform (PS1ExactT)
        ++s_ps1GeneralN;
#if PS1_PROFILE
        uint32_t gt0 = PS1ProfTicks();
        int gflag    = face->flag & 7;
#endif
#else
        memset(quad, 0, 4 * sizeof(Vertex)); // PS1: every case sets the fields its draw call reads
#endif
        switch (face->flag) {
            default: break;
            case FACE_FLAG_TEXTURED_3D:
#if RETRO_PLATFORM == RETRO_PS1
                if (PS1_ZPOS(face->a) && PS1_ZPOS(face->b) && PS1_ZPOS(face->c) && PS1_ZPOS(face->d)) {
#else
                if (vertexBufferT[face->a].z > 0 && vertexBufferT[face->b].z > 0 && vertexBufferT[face->c].z > 0 && vertexBufferT[face->d].z > 0) {
#endif
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
#if RETRO_PLATFORM == RETRO_PS1
                if (PS1_ZPOS(face->a) && PS1_ZPOS(face->b) && PS1_ZPOS(face->c) && PS1_ZPOS(face->d)) {
#else
                if (vertexBufferT[face->a].z > 0 && vertexBufferT[face->b].z > 0 && vertexBufferT[face->c].z > 0 && vertexBufferT[face->d].z > 0) {
#endif
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
#if RETRO_PLATFORM == RETRO_PS1
                if (PS1_ZPOS(face->a) && PS1_ZPOS(face->b) && PS1_ZPOS(face->c) && PS1_ZPOS(face->d)) {
#else
                if (vertexBufferT[face->a].z > 0 && vertexBufferT[face->b].z > 0 && vertexBufferT[face->c].z > 0 && vertexBufferT[face->d].z > 0) {
#endif
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
#if RETRO_PLATFORM == RETRO_PS1
                    if ((g_ps1DrawOrder[i].depth - 0x8000) >> 8 >= 0)
                        fogStr = (g_ps1DrawOrder[i].depth - 0x8000) >> 8;
#else
                    if ((drawList3D[i].depth - 0x8000) >> 8 >= 0)
                        fogStr = (drawList3D[i].depth - 0x8000) >> 8;
#endif
                    if (fogStr > fogStrength)
                        fogStr = fogStrength;

                    DrawFadedFace(quad, face->color, fogColor, 0xFF - fogStr);
                }
                break;
#if RETRO_PLATFORM == RETRO_PS1
            case FACE_FLAG_TEXTURED_C:
            case FACE_FLAG_TEXTURED_C_BLEND: { // upstream's two cases (#else), the corner's exact transform read once
                PS1VertexXYZ ta = PS1ExactT(face->a);
                if (ta.z > 0) {
                    const Vertex &vb = vertexBuffer[face->b], &va = vertexBuffer[face->a], &vc = vertexBuffer[face->c];
                    PS1TexturedRect(SCREEN_CENTERX + projectionX * (ta.x - vb.u) / ta.z, SCREEN_CENTERY - projectionY * (ta.y + vb.v) / ta.z,
                                    SCREEN_CENTERX + projectionX * (ta.x + vb.u) / ta.z, SCREEN_CENTERY - projectionY * (ta.y - vb.v) / ta.z,
                                    va.u - vc.u, va.v - vc.v, va.u + vc.u, va.v + vc.v, spriteSheetID, face->flag == FACE_FLAG_TEXTURED_C_BLEND);
                }
                break;
            }
#else
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
#endif
            case FACE_FLAG_3DSPRITE:
#if RETRO_PLATFORM == RETRO_PS1
                if (PS1VertexXYZ ta = PS1ExactT(face->a); ta.z > 0) {
                    int xpos = SCREEN_CENTERX + projectionX * ta.x / ta.z;
                    int ypos = SCREEN_CENTERY - projectionY * ta.y / ta.z;
#else
                if (vertexBufferT[face->a].z > 0) {
                    int xpos = SCREEN_CENTERX + projectionX * vertexBufferT[face->a].x / vertexBufferT[face->a].z;
                    int ypos = SCREEN_CENTERY - projectionY * vertexBufferT[face->a].y / vertexBufferT[face->a].z;
#endif

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
#if RETRO_PLATFORM == RETRO_PS1 && PS1_PROFILE
        g_ps1Prof3DGenTicks[gflag] += (PS1ProfTicks() - gt0) & 0xFFFF;
        g_ps1Prof3DGenCount[gflag]++;
#endif
    }
#if RETRO_PLATFORM == RETRO_PS1
#undef PS1_PROJECT_QUAD
#undef PS1_ZPOS
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
