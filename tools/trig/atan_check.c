// Host check (docs/30 "Mania lessons pass"): ArcTanLookup from the 64 KB table (upstream's atan2f loop) vs the PS1's
// triangle (RSDKv4/Math.cpp PS1ArcTan256), over a grid and 2,000,000 random inputs: must print 0 mismatches.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
typedef unsigned char byte;
#include "trig_tables.inc" // build/gen (make): cc -O2 -I build/gen -o build/gen/atan_check tools/trig/atan_check.c && build/gen/atan_check
static byte full[0x10000];
static byte tri(int x, int y) { if (x <= y) return arcTan256Triangle[(y * (y + 1) >> 1) + x]; return y == 0 ? 0 : (byte)(63 - arcTan256Triangle[(x * (x + 1) >> 1) + y]); }
static byte lookup(int X, int Y, int useTri) {
    int x = abs(X), y = abs(Y);
    if (x <= y) { while (y > 0xFF) { x >>= 4; y >>= 4; } } else { while (x > 0xFF) { x >>= 4; y >>= 4; } }
    byte t = useTri ? tri(x, y) : full[(x << 8) + y];
    if (X <= 0) { if (Y <= 0) return t + -0x80; else return -0x80 - t; }
    else if (Y <= 0) return -t; else return t;
}
int main(void) {
    for (int Y = 0; Y < 0x100; ++Y) { byte *a = &full[Y]; for (int X = 0; X < 0x100; ++X) { float an = atan2f(Y, X); *a = (an * 40.743664f); a += 0x100; } }
    long n = 0, bad = 0;
    for (int X = -4096; X <= 4096; ++X) for (int Y = -4096; Y <= 4096; Y += 3) { ++n; if (lookup(X, Y, 0) != lookup(X, Y, 1)) ++bad; }
    srand(1); for (int i = 0; i < 2000000; ++i) { int X = rand() - RAND_MAX / 2, Y = rand() - RAND_MAX / 2; ++n; if (lookup(X, Y, 0) != lookup(X, Y, 1)) ++bad; }
    printf("ArcTanLookup cases %ld, mismatches %ld\n", n, bad);
    return bad != 0;
}
