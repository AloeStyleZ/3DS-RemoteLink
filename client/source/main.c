// Cliente de streaming 3DS.
//   1) Handshake TCP con el PC (negocia resolucion/codec/puertos).
//   2) Cada frame: captura input (libctru) -> envia por UDP -> drena video ->
//      reensambla -> Y2R -> dibuja con citro2d.
//   3) START+SELECT abre un menu (Configurar / Salir).
//   4) Modo Juego (tactil = stick derecho + R3) o Escritorio (tactil = trackpad).
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "protocol.h"
#include "config.h"
#include "net.h"
#include "decoder.h"

// Estados de la app.
enum { ST_STREAM, ST_MENU, ST_CONFIG };

// Ajustes del trackpad (modo escritorio).
#define TRACKPAD_SENS  2    // multiplicador de movimiento (px raton por px tactil)
#define TAP_MAX_FRAMES 12   // toque mas corto que esto + poco movimiento = clic
#define TAP_MAX_MOVE   10

static inline bool in_rect(int px, int py, float x, float y, float w, float h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void stream_loop(const ServerHello* sh) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    C3D_RenderTarget* top    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    const u32 black   = C2D_Color32(0x00, 0x00, 0x00, 0xFF);
    const u32 padBg   = C2D_Color32(0x1c, 0x1c, 0x24, 0xFF);
    const u32 padBase = C2D_Color32(0x3a, 0x40, 0x52, 0xFF);
    const u32 padKnob = C2D_Color32(0x7a, 0xc0, 0xff, 0xFF);
    const u32 padSel  = C2D_Color32(0x2e, 0x6f, 0xd0, 0xFF);
    const u32 white   = C2D_Color32(0xff, 0xff, 0xff, 0xFF);
    const u32 dim     = C2D_Color32(0x88, 0x90, 0xa0, 0xFF);

    // Etiquetas de texto (fuente del sistema por defecto).
    C2D_TextBuf textBuf = C2D_TextBufNew(1024);
    C2D_Text tMenu, tConfig, tExit, tBack, tModeGame, tModeDesk, tTrack, tTrackHint, tHint;
    C2D_TextParse(&tMenu,      textBuf, "MENU");
    C2D_TextParse(&tConfig,    textBuf, "Configurar");
    C2D_TextParse(&tExit,      textBuf, "Salir");
    C2D_TextParse(&tBack,      textBuf, "Volver");
    C2D_TextParse(&tModeGame,  textBuf, "Modo: Juego");
    C2D_TextParse(&tModeDesk,  textBuf, "Modo: Escritorio");
    C2D_TextParse(&tTrack,     textBuf, "Trackpad");
    C2D_TextParse(&tTrackHint, textBuf, "Arrastra: mover   /   Toca: clic");
    C2D_TextParse(&tHint,      textBuf, "START+SELECT: volver al juego");
    C2D_TextOptimize(&tMenu);     C2D_TextOptimize(&tConfig);    C2D_TextOptimize(&tExit);
    C2D_TextOptimize(&tBack);     C2D_TextOptimize(&tModeGame);  C2D_TextOptimize(&tModeDesk);
    C2D_TextOptimize(&tTrack);    C2D_TextOptimize(&tTrackHint); C2D_TextOptimize(&tHint);

    VideoDecoder dec;
    if (!video_init(&dec, sh->width, sh->height, sh->tile_w, sh->tile_h)) {
        printf("video_init fallo\n");
        C2D_TextBufDelete(textBuf); C2D_Fini(); C3D_Fini();
        return;
    }

    // Botones (pantalla inferior 320x240). Se reutilizan en MENU y CONFIG.
    const float BW = 220, BH = 48;
    const float B0X = 50, B0Y = 75;    // Configurar / Modo
    const float B1X = 50, B1Y = 140;   // Salir / Volver

    int  state     = ST_STREAM;
    int  menuSel   = 0;     // MENU: 0=Configurar 1=Salir
    int  cfgSel    = 0;     // CONFIG: 0=Modo 1=Volver
    int  inputMode = INPUT_MODE_GAME;
    int  engaged   = 0;     // tactil en juego: 0=nada 1=stick 2=R3
    u16  inputSeq  = 0;
    bool prevCombo = false;

    // Estado del trackpad (modo escritorio).
    int  lastTpx = 0, lastTpy = 0, touchFrames = 0, movedTotal = 0, clickFrames = 0;
    bool touchActive = false;

    while (aptMainLoop()) {
        hidScanInput();
        const u32 held    = hidKeysHeld();
        const u32 kDown   = hidKeysDown();
        touchPosition tp; hidTouchRead(&tp);
        const bool touched   = (held  & KEY_TOUCH) != 0;
        const bool touchDown = (kDown & KEY_TOUCH) != 0;

        const bool combo = (held & KEY_START) && (held & KEY_SELECT);
        if (combo && !prevCombo)
            state = (state == ST_STREAM) ? ST_MENU : ST_STREAM;
        prevCombo = combo;

        float kx = (float)VSTICK_CX, ky = (float)VSTICK_CY;
        bool  r3 = false;

        if (state == ST_STREAM) {
            circlePosition cp; hidCircleRead(&cp);
            s16 rx = 0, ry = 0;
            u8  vb = 0;

            if (inputMode == INPUT_MODE_GAME) {
                // ----- Tactil = stick derecho + R3 -----
                if (touchDown) {
                    int sdx = (int)tp.px - VSTICK_CX, sdy = (int)tp.py - VSTICK_CY;
                    int bdx = (int)tp.px - VR3_CX,    bdy = (int)tp.py - VR3_CY;
                    if (sdx*sdx + sdy*sdy <= VSTICK_R*VSTICK_R)  engaged = 1;
                    else if (bdx*bdx + bdy*bdy <= VR3_R*VR3_R)   engaged = 2;
                    else                                         engaged = 0;
                }
                if (!touched) engaged = 0;
                if (engaged == 1) {
                    float dx = (float)((int)tp.px - VSTICK_CX);
                    float dy = (float)((int)tp.py - VSTICK_CY);
                    float d2 = dx*dx + dy*dy;
                    if (d2 > (float)(VSTICK_R*VSTICK_R)) { float m=(float)VSTICK_R/sqrtf(d2); dx*=m; dy*=m; }
                    rx = (s16)(dx * 32767.0f / VSTICK_R);
                    ry = (s16)(-dy * 32767.0f / VSTICK_R);
                    kx = VSTICK_CX + dx; ky = VSTICK_CY + dy;
                } else if (engaged == 2) {
                    r3 = true;
                }
                vb = r3 ? VBTN_R3 : 0;
            } else {
                // ----- Tactil = trackpad relativo (raton) -----
                if (touchDown) { lastTpx=tp.px; lastTpy=tp.py; touchActive=true; touchFrames=0; movedTotal=0; }
                if (touched && touchActive) {
                    int dx = (int)tp.px - lastTpx, dy = (int)tp.py - lastTpy;
                    rx = (s16)(dx * TRACKPAD_SENS);
                    ry = (s16)(dy * TRACKPAD_SENS);
                    movedTotal += (dx<0?-dx:dx) + (dy<0?-dy:dy);
                    lastTpx = tp.px; lastTpy = tp.py; touchFrames++;
                }
                if (!touched && touchActive) {
                    touchActive = false;
                    if (touchFrames < TAP_MAX_FRAMES && movedTotal < TAP_MAX_MOVE) clickFrames = 3;
                }
                if (clickFrames > 0) { vb |= VBTN_LCLICK; clickFrames--; }
            }

            InputPacket ip;
            ip.magic = PROTO_MAGIC; ip.type = PKT_INPUT; ip.seq = inputSeq++;
            ip.buttons = held; ip.circle_x = cp.dx; ip.circle_y = cp.dy;
            ip.right_x = rx; ip.right_y = ry; ip.vbuttons = vb; ip.mode = (u8)inputMode;
            net_send_input(&ip);
        } else {
            // ----- En menu: input NEUTRO (conserva el modo para el cursor) -----
            engaged = 0; touchActive = false; clickFrames = 0;
            InputPacket ip; memset(&ip, 0, sizeof(ip));
            ip.magic = PROTO_MAGIC; ip.type = PKT_INPUT; ip.seq = inputSeq++; ip.mode = (u8)inputMode;
            net_send_input(&ip);

            if (state == ST_MENU) {
                if (kDown & KEY_DUP)   menuSel = 0;
                if (kDown & KEY_DDOWN) menuSel = 1;
                bool act = (kDown & KEY_A) != 0;
                if (touchDown) {
                    if      (in_rect(tp.px, tp.py, B0X, B0Y, BW, BH)) { menuSel = 0; act = true; }
                    else if (in_rect(tp.px, tp.py, B1X, B1Y, BW, BH)) { menuSel = 1; act = true; }
                }
                if (act) { if (menuSel == 0) { state = ST_CONFIG; cfgSel = 0; } else break; }
                if (kDown & KEY_B) state = ST_STREAM;
            } else { // ST_CONFIG
                if (kDown & KEY_DUP)   cfgSel = 0;
                if (kDown & KEY_DDOWN) cfgSel = 1;
                bool act = (kDown & KEY_A) != 0;
                if (touchDown) {
                    if      (in_rect(tp.px, tp.py, B0X, B0Y, BW, BH)) { cfgSel = 0; act = true; }
                    else if (in_rect(tp.px, tp.py, B1X, B1Y, BW, BH)) { cfgSel = 1; act = true; }
                }
                if (act) {
                    if (cfgSel == 0)
                        inputMode = (inputMode == INPUT_MODE_GAME) ? INPUT_MODE_DESKTOP : INPUT_MODE_GAME;
                    else state = ST_MENU;
                }
                if (kDown & KEY_B) state = ST_MENU;
            }
        }

        // ----- Video (siempre, para mantener viva la conexion) -----
        net_drain_video(&dec);
        video_update(&dec);

        // ----- Render -----
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, black);
        C2D_SceneBegin(top);
        video_draw(&dec, 0.0f, 0.0f);

        C2D_TargetClear(bottom, padBg);
        C2D_SceneBegin(bottom);
        if (state == ST_STREAM) {
            if (inputMode == INPUT_MODE_GAME) {
                C2D_DrawCircleSolid((float)VSTICK_CX, (float)VSTICK_CY, 0.0f, (float)VSTICK_R, padBase);
                C2D_DrawCircleSolid(kx, ky, 0.0f, (float)VSTICK_R * 0.42f, padKnob);
                C2D_DrawCircleSolid((float)VR3_CX, (float)VR3_CY, 0.0f, (float)VR3_R, r3 ? padKnob : padBase);
            } else {
                C2D_DrawRectSolid(10, 30, 0, 300, 180, padBase);   // area del trackpad
                C2D_DrawText(&tTrack,     C2D_WithColor | C2D_AlignCenter, 160, 8,   0, 0.6f, 0.6f, white);
                C2D_DrawText(&tTrackHint, C2D_WithColor | C2D_AlignCenter, 160, 216, 0, 0.42f, 0.42f, dim);
                if (touched) C2D_DrawCircleSolid((float)tp.px, (float)tp.py, 0.0f, 6.0f, padKnob);
            }
        } else if (state == ST_MENU) {
            C2D_DrawText(&tMenu, C2D_WithColor | C2D_AlignCenter, 160, 22, 0, 0.9f, 0.9f, white);
            C2D_DrawRectSolid(B0X, B0Y, 0, BW, BH, menuSel == 0 ? padSel : padBase);
            C2D_DrawText(&tConfig, C2D_WithColor | C2D_AlignCenter, B0X + BW/2, B0Y + 13, 0, 0.7f, 0.7f, white);
            C2D_DrawRectSolid(B1X, B1Y, 0, BW, BH, menuSel == 1 ? padSel : padBase);
            C2D_DrawText(&tExit, C2D_WithColor | C2D_AlignCenter, B1X + BW/2, B1Y + 13, 0, 0.7f, 0.7f, white);
            C2D_DrawText(&tHint, C2D_WithColor | C2D_AlignCenter, 160, 215, 0, 0.45f, 0.45f, dim);
        } else { // ST_CONFIG
            C2D_DrawText(&tConfig, C2D_WithColor | C2D_AlignCenter, 160, 22, 0, 0.9f, 0.9f, white);
            C2D_DrawRectSolid(B0X, B0Y, 0, BW, BH, cfgSel == 0 ? padSel : padBase);
            C2D_DrawText(inputMode == INPUT_MODE_GAME ? &tModeGame : &tModeDesk,
                         C2D_WithColor | C2D_AlignCenter, B0X + BW/2, B0Y + 13, 0, 0.7f, 0.7f, white);
            C2D_DrawRectSolid(B1X, B1Y, 0, BW, BH, cfgSel == 1 ? padSel : padBase);
            C2D_DrawText(&tBack, C2D_WithColor | C2D_AlignCenter, B1X + BW/2, B1Y + 13, 0, 0.7f, 0.7f, white);
        }
        C3D_FrameEnd(0);
    }

    video_exit(&dec);
    C2D_TextBufDelete(textBuf);
    C2D_Fini();
    C3D_Fini();
}

int main(void) {
    gfxInitDefault();
    // No usamos consoleInit: citro2d controla ambas pantallas. Los logs de
    // arranque/handshake se ven en la consola del servidor (PC).
    printf("3DSStreaming client\nServidor: %s\n", SERVER_IP);

    if (net_init()) {
        ServerHello sh;
        if (net_handshake(SERVER_IP, &sh)) {
            printf("OK: %ux%u codec=%u tiles=%ux%u fps=%u\n",
                   sh.width, sh.height, sh.codec, sh.tiles_x, sh.tiles_y, sh.fps);
            if (sh.codec != CODEC_RAW_YUV420 && sh.codec != CODEC_JPEG_YCBCR) {
                printf("Codec no soportado: %u\n", sh.codec);
            } else if (net_open_streams(SERVER_IP, &sh)) {
                stream_loop(&sh);   // "Salir" sale de aqui -> cierra a HOME
            } else {
                printf("No se pudieron abrir los sockets UDP.\n");
            }
        } else {
            printf("Handshake fallo. PC arrancado y misma LAN?\n");
        }
        net_exit();
    } else {
        printf("soc init fallo.\n");
    }

    gfxExit();
    return 0;
}
