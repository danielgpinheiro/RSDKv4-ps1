#ifndef DRAWING3D_H
#define DRAWING3D_H

#if RETRO_PLATFORM == RETRO_PS1
// PS1: Sonic 2's half-pipe builds its mesh from script tables each frame: 1,408 vertices and 742 faces (measured,
// docs/30 phase 2.3), plus vertices 4094/4095 as single-point scratch. Script vertex indices 0xFF0-0xFFF map to the
// last 16 slots (PS1_VTX_SCRATCH); other accesses and counts past these are clamped and counted
// (g_ps1Scene3DOverflow, Script.cpp).
#define PS1_VTX_SCRATCH   (0x10)
#define VERTEXBUFFER_SIZE (0x600 + PS1_VTX_SCRATCH)
#define FACEBUFFER_SIZE   (0x340)
#else
#define VERTEXBUFFER_SIZE (0x1000)
#define FACEBUFFER_SIZE   (0x400)
#endif

enum FaceFlags {
    FACE_FLAG_TEXTURED_3D      = 0,
    FACE_FLAG_TEXTURED_2D      = 1,
    FACE_FLAG_COLORED_3D       = 2,
    FACE_FLAG_COLORED_2D       = 3,
    FACE_FLAG_FADED            = 4,
    FACE_FLAG_TEXTURED_C       = 5,
    FACE_FLAG_TEXTURED_C_BLEND = 6,
    FACE_FLAG_3DSPRITE         = 7
};

enum MatrixTypes {
    MAT_WORLD = 0,
    MAT_VIEW  = 1,
    MAT_TEMP  = 2,
};

struct Matrix {
    int values[4][4];
};

struct Vertex {
    int x;
    int y;
    int z;
    int u;
    int v;
};

struct Face {
    int a;
    int b;
    int c;
    int d;
    uint color;
    int flag;
};

struct DrawListEntry3D {
    int faceID;
    int depth;
};

extern int vertexCount;
extern int faceCount;

extern Matrix matFinal;
extern Matrix matWorld;
extern Matrix matView;
extern Matrix matTemp;

extern Face faceBuffer[FACEBUFFER_SIZE];
extern Vertex vertexBuffer[VERTEXBUFFER_SIZE];
extern Vertex vertexBufferT[VERTEXBUFFER_SIZE];

extern DrawListEntry3D drawList3D[FACEBUFFER_SIZE];

extern int projectionX;
extern int projectionY;
extern int fogColor;
extern int fogStrength;

extern int faceLineStart[SCREEN_YSIZE];
extern int faceLineEnd[SCREEN_YSIZE];
extern int faceLineStartU[SCREEN_YSIZE];
extern int faceLineEndU[SCREEN_YSIZE];
extern int faceLineStartV[SCREEN_YSIZE];
extern int faceLineEndV[SCREEN_YSIZE];

void SetIdentityMatrix(Matrix *matrix);
void MatrixMultiply(Matrix *matrixA, Matrix *matrixB);
void MatrixTranslateXYZ(Matrix *Matrix, int x, int y, int z);
void MatrixScaleXYZ(Matrix *matrix, int scaleX, int scaleY, int scaleZ);
void MatrixRotateX(Matrix *matrix, int rotationX);
void MatrixRotateY(Matrix *matrix, int rotationY);
void MatrixRotateZ(Matrix *matrix, int rotationZ);
void MatrixRotateXYZ(Matrix *matrix, short rotationX, short rotationY, short rotationZ);
#if !RETRO_REV00
void MatrixInverse(Matrix *matrix);
#endif
void TransformVertexBuffer();
void TransformVertices(Matrix *matrix, int startIndex, int endIndex);
void Sort3DDrawList();
void Draw3DScene(int spriteSheetID);

void ProcessScanEdge(Vertex *vertA, Vertex *vertB);
void ProcessScanEdgeUV(Vertex *vertA, Vertex *vertB);

#endif // !DRAWING3D_H
