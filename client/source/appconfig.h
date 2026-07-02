// Ajustes persistentes del cliente (se guardan en la SD).
#pragma once
#include <3ds.h>

typedef struct {
    char serverIp[64];
    int  quality;     // 10..95 (calidad en movimiento que pide el cliente)
    int  fps;         // 10..60
    int  inputMode;   // 0=juego, 1=escritorio
    int  buttonSwap;  // 0/1 (A/B y X/Y por posicion en vez de por nombre)
    int  sensPercent; // 50..200 (sensibilidad del circle pad)
    int  deadzone;    // 0..40 (zona muerta del circle pad)
    int  fpsHud;      // 0/1 (overlay de FPS)
} AppConfig;

void appconfig_load(AppConfig* c);   // rellena con defaults si no hay fichero
void appconfig_save(const AppConfig* c);
