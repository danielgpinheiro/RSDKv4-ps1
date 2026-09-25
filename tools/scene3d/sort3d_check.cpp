// Host check (docs/30 phase 2.3; from the Sonic CD port): the PS1 merge sort of the 3D draw list (RSDKv4/Scene3D.cpp,
// PS1SortDrawList3D) gives exactly upstream's bubble-sort order, on random depths with many ties.
// Run: tools/scene3d/sort3d_check.sh (extracts the function, builds and runs this).
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#include <cstring>
#define FACEBUFFER_SIZE (0x400)
struct DrawListEntry3D {
    int faceID;
    int depth;
};
#define PS1_SORT3D_CHECK
#define RETRO_PLATFORM 0
#define RETRO_PS1 1
// the function under test, extracted verbatim from Scene3D.cpp
#include SORT3D_EXTRACT

static void bubble(DrawListEntry3D *l, int n) { // upstream Sort3DDrawList, sorting part
    for (int i = 0; i < n; ++i)
        for (int j = n - 1; j > i; --j)
            if (l[j].depth > l[j - 1].depth) {
                DrawListEntry3D t = l[j];
                l[j] = l[j - 1];
                l[j - 1] = t;
            }
}

int main() {
    static DrawListEntry3D a[FACEBUFFER_SIZE], b[FACEBUFFER_SIZE];
    int cases = 0;
    for (int n : {0, 1, 2, 3, 7, 64, 159, 160, 161, 256, 741, 742, 743, 1024})
        for (int range : {1, 3, 50, 100000})
            for (int rep = 0; rep < 200; ++rep, ++cases) {
                for (int i = 0; i < n; ++i) a[i] = b[i] = {i, rand() % range - range / 2};
                bubble(a, n);
                PS1SortDrawList3D(b, n);
                if (memcmp(a, b, n * sizeof(*a))) {
                    printf("MISMATCH n=%d range=%d\n", n, range);
                    return 1;
                }
            }
    printf("sort3d: %d cases identical to the bubble sort\n", cases);
    return 0;
}
