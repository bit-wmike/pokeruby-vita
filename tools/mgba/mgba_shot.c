/* Original ROM in mGBA (libmgba 0.10), headless: the reference for the port.
 * mgba_shot rom sav frames "frame:keys:len,..." "f1,f2,..." outdir
 * keys: GBA KEYINPUT bits (A=1 B=2 SELECT=4 START=8 RIGHT=16 LEFT=32 UP=64 DOWN=128 R=256 L=512) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba-util/vfs.h>

int main(int argc, char **argv)
{
    static unsigned buf[256 * 160];
    struct mCore *core;
    int frames, f, nk = 0, ns = 0, i;
    int kf[512], kk[512], kl[512], shots[512];
    char *p;
    if (argc < 7) return 1;
    core = mCoreFind(argv[1]);
    if (!core) { fprintf(stderr, "no core\n"); return 1; }
    core->init(core);
    mCoreInitConfig(core, NULL);
    core->setVideoBuffer(core, (void *)buf, 256);
    if (!mCoreLoadFile(core, argv[1])) { fprintf(stderr, "rom\n"); return 1; }
    {
        struct VFile *vf = VFileOpen(argv[2], O_RDWR);
        if (!vf || !core->loadSave(core, vf)) { fprintf(stderr, "save\n"); return 1; }
    }
    core->reset(core);
    frames = atoi(argv[3]);
    for (p = argv[4]; *p && nk < 512;) {
        if (sscanf(p, "%d:%x:%d", &kf[nk], &kk[nk], &kl[nk]) == 3) nk++;
        p = strchr(p, ','); if (!p) break; p++;
    }
    for (p = argv[5]; *p && ns < 512;) {
        shots[ns++] = atoi(p);
        p = strchr(p, ','); if (!p) break; p++;
    }
    for (f = 0; f < frames; f++) {
        unsigned keys = 0;
        for (i = 0; i < nk; i++)
            if (f >= kf[i] && f < kf[i] + kl[i]) keys |= kk[i];
        core->setKeys(core, keys);
        core->runFrame(core);
        for (i = 0; i < ns; i++)
            if (shots[i] == f) {
                char name[512];
                FILE *o;
                int x, y;
                snprintf(name, sizeof(name), "%s/mgba_%05d.ppm", argv[6], f);
                o = fopen(name, "wb");
                fprintf(o, "P6\n240 160\n255\n");
                for (y = 0; y < 160; y++)
                    for (x = 0; x < 240; x++) {
                        if (getenv("MGBA_16")) {   /* libmgba built with 16-bit colour */
                            unsigned c = ((const unsigned short *)buf)[y * 256 + x];
                            fputc((c & 31) << 3, o); fputc(((c >> 5) & 31) << 3, o); fputc(((c >> 10) & 31) << 3, o);
                        } else {
                            unsigned c = buf[y * 256 + x];
                            fputc(c & 0xFF, o); fputc((c >> 8) & 0xFF, o); fputc((c >> 16) & 0xFF, o);
                        }
                    }
                fclose(o);
            }
    }
    if (getenv("MGBA_IO")) {
        for (i = 0; i < 0x56; i += 2)
            printf("%02x=%04x ", i, core->rawRead16(core, 0x04000000 + i, -1));
        printf("\n");
    }
    if (getenv("MGBA_PAL")) {   /* BG palette RAM at the end */
        for (i = 0; i < 512; i++) {
            unsigned c = core->rawRead16(core, 0x05000000 + i * 2, -1);
            printf("%04x%c", c, (i & 15) == 15 ? '\n' : ' ');
        }
    }
    core->deinit(core);
    return 0;
}
