// Build stamp on the menus' title screen (see version_stamp.hh).
#include "version_stamp.hh"

#include <psyqo/cdrom-device.hh>
#include <psyqo/gpu.hh>

extern psyqo::GPU *g_ps1GPU;
extern psyqo::CDRomDevice *g_ps1CDRom;
bool PS1FsFind(const char *path, uint32_t *lba, uint32_t *size); // cdrom_fs.cpp

volatile uint32_t g_ps1VersionFound = 0;

namespace {
char s_text[32] = "v unknown";
}

void PS1VersionLoad() {
    uint32_t lba = 0, size = 0;
    uint32_t sec[512]; // one sector, word aligned for the CD DMA
    if (!PS1FsFind("VERSION.TXT", &lba, &size) || !size || !g_ps1CDRom->readSectorsBlocking(lba, 1, sec, *g_ps1GPU))
        return;
    const char *p = (const char *)sec;
    int n         = 2; // after "v "
    for (uint32_t i = 0; i < size && i < 2048 && p[i] != '\n' && p[i] != '\r' && p[i] && n < 2 + 16; ++i)
        s_text[n++] = p[i];
    if (n > 2) {
        s_text[n]         = 0;
        g_ps1VersionFound = 1;
    }
}

const char *PS1VersionText() { return s_text; }
