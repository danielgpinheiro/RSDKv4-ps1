#!/bin/bash
# Host check (docs/30 phase 7.2b): PS1MatrixInverseRotation (RSDKv4/Scene3D.cpp) == upstream's double MatrixInverse,
# bit for bit, on random rotations and 3x3 blocks up to +-32767 (IEEE doubles, no FMA: as the PS1's soft-float).
set -eu
cd "$(dirname "$0")/../.."
T=$(mktemp -d)
python3 - "$T" <<'PY'
import sys
s = open('RSDKv4/Scene3D.cpp').read()
i = s.index('typedef unsigned long long PS1U64;')  # the integer helpers, then PS1MatrixInverseRotation
k = s.index('static bool PS1MatrixInverseRotation')
open(sys.argv[1] + '/fast.inc', 'w').write(s[i:s.index('\n}\n', k) + 3])
j = s.index('void MatrixInverse(Matrix *matrix)')
body = s[j:s.index('\n}\n', j) + 3]
body = body.replace('void MatrixInverse(', 'static void MatrixInverseUp(')
body = body.replace('#if RETRO_PLATFORM == RETRO_PS1\n    if (PS1MatrixInverseRotation(matrix))\n        return;\n#endif\n', '')
open(sys.argv[1] + '/up.inc', 'w').write(body)
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
    return bad != 0;
}
CPP
c++ -std=c++17 -O1 -ffp-contract=off -I"$T" -o "$T/t" "$T/t.cpp"
"$T/t"
rm -rf "$T"
