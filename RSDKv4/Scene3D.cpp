#include "RetroEngine.hpp"

int vertexCount = 0;
int faceCount   = 0;

Matrix matFinal = Matrix();
Matrix matWorld = Matrix();
Matrix matView  = Matrix();
Matrix matTemp  = Matrix();

Face faceBuffer[FACEBUFFER_SIZE];
Vertex vertexBuffer[VERTEXBUFFER_SIZE];
#if RETRO_PLATFORM == RETRO_PS1
PS1ProjEntry g_ps1Proj[VERTEXBUFFER_SIZE];
PS1ExactEntry g_ps1ExactT[256];
uint g_ps1ExactFrame = 1;
#define vertexBufferT (PS1VertexTView{}) // upstream's reads below get the exact transform (PS1ExactT)
#else
Vertex vertexBufferT[VERTEXBUFFER_SIZE];
#endif

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
    g_ps1Proj[v].u = 0x8000u;
    g_ps1Proj[v].v = PS1ExactT(v).z;
}
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
    for (; v + 3 <= count; v += 3) {
        const Vertex *p = &vertexBuffer[v];
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
        PS1ProjEntry *t = &g_ps1Proj[v];
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
        const Face &f = faceBuffer[drawList3D[i].faceID];
        if (f.flag != FACE_FLAG_FADED)
            break;
        uint32_t pa = g_ps1Proj[f.a].u, pb = g_ps1Proj[f.b].u, pc = g_ps1Proj[f.c].u, pd = g_ps1Proj[f.d].u;
        // a corner left to the CPU: x = 0x8000; GTE x is -1024..1023, so x + 1024 has bits 11-15 clear
        if (((pa + 1024) | (pb + 1024) | (pc + 1024) | (pd + 1024)) & 0xF800)
            break;
        ++fast;
        int fogStr = (drawList3D[i].depth - 0x8000) >> 8;
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
            if (run)
                PS1ChainBulkEnd(used);
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
    if (run)
        PS1ChainBulkEnd(used);
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
// The draw-list sort's scratch (docs/30 phase 7; the screen positions live in vertexBufferT .u since phase 7.2b).
DrawListEntry3D g_ps1Scene3DScratch[FACEBUFFER_SIZE];
#endif
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
    for (int i = 0; i < count; ++i) list[i] = dst[i];
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
#endif
void Draw3DScene(int spriteSheetID)
{
#if RETRO_PLATFORM == RETRO_PS1
    uint32_t t0        = PS1Hblanks();
    g_ps1Prof3DProject = 0; // the projection is part of the transform now (PS1TransformProject3D)
    uint32_t fadedHbl = 0, generalN = 0;
    for (int k = 0; k < 8; ++k) g_ps1Prof3DGenTicks[k] = g_ps1Prof3DGenCount[k] = 0;
#define PS1_PROJECT_QUAD()                                                                                                                  \
    PS1ProjectedVertex(face->a, &quad[0].x, &quad[0].y);                                                                                  \
    PS1ProjectedVertex(face->b, &quad[1].x, &quad[1].y);                                                                                  \
    PS1ProjectedVertex(face->c, &quad[2].x, &quad[2].y);                                                                                  \
    PS1ProjectedVertex(face->d, &quad[3].x, &quad[3].y);
#endif
    Vertex quad[4];
    for (int i = 0; i < faceCount; ++i) {
        Face *face = &faceBuffer[drawList3D[i].faceID];
#if RETRO_PLATFORM == RETRO_PS1
        if (face->flag == FACE_FLAG_FADED) { // the fast path: the run of faded faces whose corners the GTE projected
            uint32_t tf = PS1Hblanks();
            int next    = PS1FadedFaces(i, faceCount);
            fadedHbl += PS1HblanksSince(tf);
            if (next != i) {
                i = next - 1;
                continue;
            }
        }
        // the general path: upstream's code; its vertexBufferT reads get the exact transform (PS1ExactT)
        ++generalN;
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
    g_ps1Prof3DDraw     = PS1HblanksSince(t0);
    g_ps1Prof3DFaded    = fadedHbl;
    g_ps1Prof3DGeneral  = g_ps1Prof3DDraw - fadedHbl;
    g_ps1Prof3DGeneralN = generalN;
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
