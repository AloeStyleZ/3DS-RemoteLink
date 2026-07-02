#include "appconfig.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

#define CFG_PATH  "sdmc:/3ds/3dsstream.cfg"
#define CFG_MAGIC 0x44535332u   // "DSS2"

typedef struct { u32 magic; AppConfig c; } CfgFile;

void appconfig_load(AppConfig* c) {
    // Defaults.
    memset(c, 0, sizeof(*c));
    strncpy(c->serverIp, SERVER_IP, sizeof(c->serverIp) - 1);
    c->quality = 40; c->fps = 24; c->inputMode = 0; c->buttonSwap = 0;
    c->sensPercent = 100; c->deadzone = 8; c->fpsHud = 0;

    FILE* f = fopen(CFG_PATH, "rb");
    if (!f) return;
    CfgFile cf;
    if (fread(&cf, 1, sizeof(cf), f) == sizeof(cf) && cf.magic == CFG_MAGIC) {
        *c = cf.c;
        c->serverIp[sizeof(c->serverIp) - 1] = 0;
    }
    fclose(f);
}

void appconfig_save(const AppConfig* c) {
    FILE* f = fopen(CFG_PATH, "wb");
    if (!f) return;
    CfgFile cf;
    cf.magic = CFG_MAGIC;
    cf.c = *c;
    fwrite(&cf, 1, sizeof(cf), f);
    fclose(f);
}
