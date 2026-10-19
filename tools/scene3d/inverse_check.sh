#!/bin/bash
# Host check (docs/30 phase 7.2b): PS1MatrixInverseRotation (RSDKv4/Scene3D.cpp) == upstream's double MatrixInverse,
# bit for bit, on random rotations and 3x3 blocks up to +-32767 (IEEE doubles, no FMA: as the PS1's soft-float); and
# Sonic 2's table-driven PS1MatrixInverseTable (docs/43) == upstream's on random full 4x4 matrices (any last column and
# translation, singular ones included).
set -eu
cd "$(dirname "$0")/../.."
T=$(mktemp -d)
python3 - "$T" <<'PY'
import sys
s = open('RSDKv4/Scene3D.cpp').read()
i = s.index('typedef unsigned long long PS1U64;')  # the integer helpers, then PS1MatrixInverseRotation
k = s.index('static bool PS1MatrixInverseRotation')
open(sys.argv[1] + '/fast.inc', 'w').write(s[i:s.index('\n}\n', k) + 3].replace('PS1_S2_SMALL ', ''))
j = s.index('void MatrixInverse(Matrix *matrix)')
body = s[j:s.index('\n}\n', j) + 3]
body = body.replace('void MatrixInverse(', 'static void MatrixInverseUp(')
body = body.replace('#if RETRO_PLATFORM == RETRO_PS1\n    if (PS1MatrixInverseRotation(matrix))\n        return;\n#endif\n', '')
body = body.replace('#ifdef PS1_INV_TABLE\n    PS1MatrixInverseTable(matrix);\n#else\n', '#if 1\n')
open(sys.argv[1] + '/up.inc', 'w').write(body)
t = s.index('static const uint8_t s_ps1InvTerm')
tb = s[t:s.index('\n}\n', s.index('static void PS1MatrixInverseTable', t)) + 3].replace('PS1_S2_SMALL ', '')
open(sys.argv[1] + '/table.inc', 'w').write(tb)
PY
cat > "$T/t.cpp" <<'CPP'
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
struct Matrix { int values[4][4]; };
#include "up.inc"
#include "fast.inc"
#include "table.inc"
int main() {
    long bad = 0, n = 0;
    srand(1);
    auto rnd = [](int r) { return (int)(((unsigned)rand() << 8 ^ (unsigned)rand()) % (2u * r + 1)) - r; };
    for (int it = 0; it < 3000000; ++it) {
        Matrix a{}, b{};
        int R = it % 3 == 0 ? 300 : it % 3 == 1 ? 4096 : 32767;
        for (int y = 0; y < 3; ++y) for (int x = 0; x < 3; ++x) a.values[y][x] = rnd(R);
        if (it % 5 == 0) {
            double ax = rnd(314) / 100.0, ay = rnd(314) / 100.0;
            a.values[0][0] = (int)(cos(ay) * 256); a.values[0][2] = (int)(sin(ay) * 256); a.values[2][0] = -(int)(sin(ay) * 256);
            a.values[2][2] = (int)(cos(ay) * 256); a.values[1][1] = (int)(cos(ax) * 256);
            a.values[0][1] = a.values[1][0] = a.values[1][2] = a.values[2][1] = 0;
        }
        a.values[3][3] = 256;
        b = a;
        MatrixInverseUp(&a);
        if (!PS1MatrixInverseRotation(&b)) { printf("fast path refused\n"); return 1; }
        ++n;
        bad += memcmp(&a, &b, sizeof(a)) != 0;
    }
    printf("inverse: %ld cases, %ld differ from upstream's double MatrixInverse\n", n, bad);
    long tbad = 0, tn = 0, sing = 0;
    for (int it = 0; it < 3000000; ++it) {
        Matrix a{}, b{};
        int R = it % 4 == 0 ? 2 : it % 4 == 1 ? 300 : it % 4 == 2 ? 4096 : 1 << 20;
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) a.values[y][x] = rnd(R);
        if (it % 7 == 0) a.values[0][3] = a.values[1][3] = a.values[2][3] = 0, a.values[3][3] = 256; // affine
        if (it % 11 == 0) for (int x = 0; x < 4; ++x) a.values[2][x] = a.values[1][x];                 // singular
        b = a;
        MatrixInverseUp(&a);
        PS1MatrixInverseTable(&b);
        ++tn;
        sing += memcmp(&a, &b, sizeof(a)) == 0 && it % 11 == 0;
        tbad += memcmp(&a, &b, sizeof(a)) != 0;
    }
    printf("table inverse: %ld cases (%ld singular), %ld differ from upstream's double MatrixInverse\n", tn, sing, tbad);
    return bad != 0 || tbad != 0;
}
CPP
c++ -std=c++17 -O1 -ffp-contract=off -I"$T" -o "$T/t" "$T/t.cpp"
"$T/t"
rm -rf "$T"
