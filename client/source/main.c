// Cliente de streaming 3DS.
//   - Graficos (citro3d/2d) inicializados UNA vez en main; reconexion limpia.
//   - Input: gamepad (swap A/B/X/Y, sens/zona muerta), stick/raton tactil, teclado.
//   - Menu START+SELECT con ajustes en vivo, guardados en la SD.
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "protocol.h"
#include "config.h"
#include "net.h"
#include "decoder.h"
#include "appconfig.h"

enum { ST_STREAM, ST_MENU, ST_CONFIG };
enum { SL_QUIT, SL_DISCONNECT };

#define TRACKPAD_SENS  2
#define TAP_MAX_FRAMES 12
#define TAP_MAX_MOVE   10
#define DOUBLE_TAP_MS  350
#define CFG_ROWS       9
#define RX_TIMEOUT_MS  4000

typedef struct {
    C3D_RenderTarget *top, *bottom;
    C2D_TextBuf       textBuf, dynBuf;   // persistentes entre conexiones
} Ui;

static inline bool in_rect(int px, int py, float x, float y, float w, float h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static bool swkbd_get(char* out, int outsz, const char* hint, const char* initial) {
    SwkbdState s;
    swkbdInit(&s, SWKBD_TYPE_NORMAL, 2, outsz - 1);
    if (initial && initial[0]) swkbdSetInitialText(&s, initial);
    swkbdSetHintText(&s, hint);
    out[0] = 0;
    SwkbdButton b = swkbdInputText(&s, out, outsz);
    return b == SWKBD_BUTTON_RIGHT && out[0] != 0;
}

static void ui_init(Ui* u) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    u->top     = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    u->bottom  = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    u->textBuf = C2D_TextBufNew(1024);
    u->dynBuf  = C2D_TextBufNew(256);
}
static void ui_exit(Ui* u) {
    C2D_TextBufDelete(u->dynBuf);
    C2D_TextBufDelete(u->textBuf);
    C2D_Fini();
    C3D_Fini();
}

static void draw_connecting(Ui* u, const char* ip) {
    char s[96]; snprintf(s, sizeof(s), "Conectando a %s ...", ip);
    C2D_TextBufClear(u->dynBuf);
    C2D_Text t; C2D_TextParse(&t, u->dynBuf, s); C2D_TextOptimize(&t);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(u->top, C2D_Color32(0, 0, 0, 0xFF));
    C2D_SceneBegin(u->top);
    C2D_DrawText(&t, C2D_WithColor | C2D_AlignCenter, 200, 108, 0, 0.6f, 0.6f, C2D_Color32(0xff,0xff,0xff,0xFF));
    C2D_TargetClear(u->bottom, C2D_Color32(0x1c, 0x1c, 0x24, 0xFF));
    C2D_SceneBegin(u->bottom);
    C3D_FrameEnd(0);
}

static int stream_loop(Ui* ui, AppConfig* cfg, const ServerHello* sh) {
    C3D_RenderTarget* top    = ui->top;
    C3D_RenderTarget* bottom = ui->bottom;
    C2D_TextBuf       dynBuf  = ui->dynBuf;

    const u32 black   = C2D_Color32(0x00, 0x00, 0x00, 0xFF);
    const u32 padBg   = C2D_Color32(0x1c, 0x1c, 0x24, 0xFF);
    const u32 padBase = C2D_Color32(0x3a, 0x40, 0x52, 0xFF);
    const u32 padKnob = C2D_Color32(0x7a, 0xc0, 0xff, 0xFF);
    const u32 padSel  = C2D_Color32(0x2e, 0x6f, 0xd0, 0xFF);
    const u32 white   = C2D_Color32(0xff, 0xff, 0xff, 0xFF);
    const u32 dim     = C2D_Color32(0x88, 0x90, 0xa0, 0xFF);
    const u32 fpsClr  = C2D_Color32(0x6e, 0xff, 0x6e, 0xFF);
    const u32 fpsBg   = C2D_Color32(0x00, 0x00, 0x00, 0xB0);

    // Etiquetas (en el textBuf persistente, que limpiamos primero).
    C2D_TextBufClear(ui->textBuf);
    C2D_Text tKbd, tConfig, tExit, tBack, tModeGame, tModeDesk, tBtnName, tBtnPos,
             tHudOn, tHudOff, tIP, tLClick, tRClick, tTrack, tTrackHint, tHint;
    C2D_TextParse(&tKbd,       ui->textBuf, "Teclado");
    C2D_TextParse(&tConfig,    ui->textBuf, "Configurar");
    C2D_TextParse(&tExit,      ui->textBuf, "Salir");
    C2D_TextParse(&tBack,      ui->textBuf, "Volver");
    C2D_TextParse(&tModeGame,  ui->textBuf, "Modo: Juego");
    C2D_TextParse(&tModeDesk,  ui->textBuf, "Modo: Escritorio");
    C2D_TextParse(&tBtnName,   ui->textBuf, "Botones: Nombre");
    C2D_TextParse(&tBtnPos,    ui->textBuf, "Botones: Posicion");
    C2D_TextParse(&tHudOn,     ui->textBuf, "FPS HUD: ON");
    C2D_TextParse(&tHudOff,    ui->textBuf, "FPS HUD: OFF");
    C2D_TextParse(&tIP,        ui->textBuf, "Cambiar IP...");
    C2D_TextParse(&tLClick,    ui->textBuf, "Clic Izq");
    C2D_TextParse(&tRClick,    ui->textBuf, "Clic Der");
    C2D_TextParse(&tTrack,     ui->textBuf, "Trackpad");
    C2D_TextParse(&tTrackHint, ui->textBuf, "Arrastra: mover  /  toca: clic izq");
    C2D_TextParse(&tHint,      ui->textBuf, "START+SELECT: volver al juego");
    C2D_Text* all[] = { &tKbd,&tConfig,&tExit,&tBack,&tModeGame,&tModeDesk,&tBtnName,
                        &tBtnPos,&tHudOn,&tHudOff,&tIP,&tLClick,&tRClick,&tTrack,&tTrackHint,&tHint };
    for (unsigned i = 0; i < sizeof(all)/sizeof(all[0]); ++i) C2D_TextOptimize(all[i]);

    VideoDecoder dec;
    if (!video_init(&dec, sh->width, sh->height, sh->tile_w, sh->tile_h))
        return SL_DISCONNECT;

    net_send_ctrl(CTRL_SET_QUALITY, (u32)cfg->quality);
    net_send_ctrl(CTRL_SET_FPS,     (u32)cfg->fps);
    net_send_ctrl(CTRL_REQUEST_KEYFRAME, 0);

    const float MX = 50, MW = 220, MH = 44, MY0 = 55, MSTEP = 52;
    const float RX = 40, RW = 240, RH = 21, RY0 = 26, RSTEP = 23;
    const float BTNY = 198, BTNH = 36;
    const float LBX = 10,  LBW = 145;   // boton Clic Izq
    const float RBX = 165, RBW = 145;   // boton Clic Der

    int  state = ST_STREAM, menuSel = 0, cfgSel = 0, engaged = 0;
    u16  inputSeq = 0;
    bool prevCombo = false;
    int  lastTpx = 0, lastTpy = 0, touchFrames = 0, movedTotal = 0, lclickFrames = 0;
    bool touchActive = false;
    int  result = SL_QUIT;

    u64  fpsT = osGetTime(), lastRx = osGetTime(), lastKeyReq = 0;
    u32  lastFrames = dec.framesPresented, lastDropped = dec.droppedTiles;
    int  videoFps = 0;

    while (aptMainLoop()) {
        hidScanInput();
        const u32 held    = hidKeysHeld();
        const u32 kDown   = hidKeysDown();
        touchPosition tp; hidTouchRead(&tp);
        const bool touched   = (held  & KEY_TOUCH) != 0;
        const bool touchDown = (kDown & KEY_TOUCH) != 0;

        const bool combo = (held & KEY_START) && (held & KEY_SELECT);
        if (combo && !prevCombo) state = (state == ST_STREAM) ? ST_MENU : ST_STREAM;
        prevCombo = combo;

        float kx = (float)VSTICK_CX, ky = (float)VSTICK_CY;
        bool  r3 = false;

        if (state == ST_STREAM) {
            circlePosition cp; hidCircleRead(&cp);
            int ax = cp.dx, ay = cp.dy;
            float mag = sqrtf((float)(ax*ax + ay*ay));
            if (mag <= cfg->deadzone) { ax = 0; ay = 0; }
            else {
                float k = (mag - cfg->deadzone) / mag * (cfg->sensPercent / 100.0f);
                ax = clampi((int)(ax * k), -156, 156);
                ay = clampi((int)(ay * k), -156, 156);
            }
            u32 b = held;
            if (cfg->buttonSwap) {
                u32 nb = b & ~(u32)(KEY_A | KEY_B | KEY_X | KEY_Y);
                if (b & KEY_A) nb |= KEY_B;
                if (b & KEY_B) nb |= KEY_A;
                if (b & KEY_X) nb |= KEY_Y;
                if (b & KEY_Y) nb |= KEY_X;
                b = nb;
            }

            s16 rx = 0, ry = 0;
            u8  vb = 0;
            if (cfg->inputMode == INPUT_MODE_GAME) {
                if (touchDown) {
                    int sdx = (int)tp.px - VSTICK_CX, sdy = (int)tp.py - VSTICK_CY;
                    int bdx = (int)tp.px - VR3_CX,    bdy = (int)tp.py - VR3_CY;
                    if (sdx*sdx + sdy*sdy <= VSTICK_R*VSTICK_R)  engaged = 1;
                    else if (bdx*bdx + bdy*bdy <= VR3_R*VR3_R)   engaged = 2;
                    else                                         engaged = 0;
                }
                if (!touched) engaged = 0;
                if (engaged == 1) {
                    float dx = (float)((int)tp.px - VSTICK_CX), dy = (float)((int)tp.py - VSTICK_CY);
                    float d2 = dx*dx + dy*dy;
                    if (d2 > (float)(VSTICK_R*VSTICK_R)) { float m=(float)VSTICK_R/sqrtf(d2); dx*=m; dy*=m; }
                    rx = (s16)(dx * 32767.0f / VSTICK_R);
                    ry = (s16)(-dy * 32767.0f / VSTICK_R);
                    kx = VSTICK_CX + dx; ky = VSTICK_CY + dy;
                } else if (engaged == 2) r3 = true;
                vb = r3 ? VBTN_R3 : 0;
            } else {
                const bool onIzq = touched && in_rect(tp.px, tp.py, LBX, BTNY, LBW, BTNH);
                const bool onDer = touched && in_rect(tp.px, tp.py, RBX, BTNY, RBW, BTNH);
                const bool onPad = touched && (int)tp.py < 190;   // trackpad = zona superior
                if (onIzq) vb |= VBTN_LCLICK;                     // boton clic izq (mantenido)
                if (onDer) vb |= VBTN_RCLICK;                     // boton clic der (mantenido)
                if (touchDown) {
                    if (onPad) { lastTpx=tp.px; lastTpy=tp.py; touchActive=true; touchFrames=0; movedTotal=0; }
                    else touchActive = false;
                }
                if (touched && touchActive) {
                    int dx = (int)tp.px - lastTpx, dy = (int)tp.py - lastTpy;
                    rx = (s16)(dx * TRACKPAD_SENS); ry = (s16)(dy * TRACKPAD_SENS);
                    movedTotal += (dx<0?-dx:dx) + (dy<0?-dy:dy);
                    lastTpx = tp.px; lastTpy = tp.py; touchFrames++;
                }
                if (!touched && touchActive) {
                    touchActive = false;
                    if (touchFrames < TAP_MAX_FRAMES && movedTotal < TAP_MAX_MOVE) lclickFrames = 3;  // tap = clic izq
                }
                if (lclickFrames > 0) { vb |= VBTN_LCLICK; lclickFrames--; }
            }

            InputPacket ip;
            ip.magic = PROTO_MAGIC; ip.type = PKT_INPUT; ip.seq = inputSeq++;
            ip.buttons = b; ip.circle_x = (s16)ax; ip.circle_y = (s16)ay;
            ip.right_x = rx; ip.right_y = ry; ip.vbuttons = vb; ip.mode = (u8)cfg->inputMode;
            net_send_input(&ip);
        } else {
            engaged = 0; touchActive = false; lclickFrames = 0;
            InputPacket ip; memset(&ip, 0, sizeof(ip));
            ip.magic = PROTO_MAGIC; ip.type = PKT_INPUT; ip.seq = inputSeq++; ip.mode = (u8)cfg->inputMode;
            net_send_input(&ip);

            if (state == ST_MENU) {
                if (kDown & KEY_DUP)   menuSel = (menuSel + 2) % 3;
                if (kDown & KEY_DDOWN) menuSel = (menuSel + 1) % 3;
                bool act = (kDown & KEY_A) != 0;
                if (touchDown)
                    for (int r = 0; r < 3; ++r)
                        if (in_rect(tp.px, tp.py, MX, MY0 + r*MSTEP, MW, MH)) { menuSel = r; act = true; break; }
                if (act) {
                    if (menuSel == 0) {
                        char tbuf[TEXT_MAX + 1];
                        if (swkbd_get(tbuf, sizeof(tbuf), "Texto para el PC", NULL))
                            net_send_text(tbuf, (int)strlen(tbuf));
                        // swkbd bloqueo el bucle: no lo cuentes como caida y pide keyframe.
                        lastRx = osGetTime();
                        net_send_ctrl(CTRL_REQUEST_KEYFRAME, 0);
                    } else if (menuSel == 1) { state = ST_CONFIG; cfgSel = 0; }
                    else break;
                }
                if (kDown & KEY_B) state = ST_STREAM;
            } else { // ST_CONFIG
                if (kDown & KEY_DUP)   cfgSel = (cfgSel + CFG_ROWS - 1) % CFG_ROWS;
                if (kDown & KEY_DDOWN) cfgSel = (cfgSel + 1) % CFG_ROWS;
                int  dir = (kDown & KEY_DRIGHT) ? 1 : ((kDown & KEY_DLEFT) ? -1 : 0);
                bool act = (kDown & KEY_A) != 0;
                if (touchDown)
                    for (int r = 0; r < CFG_ROWS; ++r)
                        if (in_rect(tp.px, tp.py, RX, RY0 + r*RSTEP, RW, RH)) { cfgSel = r; act = true; break; }

                switch (cfgSel) {
                    case 0: if (dir || act) cfg->inputMode = (cfg->inputMode == INPUT_MODE_GAME) ? INPUT_MODE_DESKTOP : INPUT_MODE_GAME; break;
                    case 1: if (dir || act) cfg->buttonSwap = !cfg->buttonSwap; break;
                    case 2: if (dir || act) cfg->fpsHud = !cfg->fpsHud; break;
                    case 3: if (dir) { cfg->quality = clampi(cfg->quality + dir*5, 10, 95); net_send_ctrl(CTRL_SET_QUALITY, (u32)cfg->quality); } break;
                    case 4: if (dir) { cfg->fps = clampi(cfg->fps + dir*5, 10, 60); net_send_ctrl(CTRL_SET_FPS, (u32)cfg->fps); } break;
                    case 5: if (dir) cfg->sensPercent = clampi(cfg->sensPercent + dir*10, 50, 200); break;
                    case 6: if (dir) cfg->deadzone = clampi(cfg->deadzone + dir*2, 0, 40); break;
                    case 7: if (act) {
                                char ipbuf[64];
                                if (swkbd_get(ipbuf, sizeof(ipbuf), "IP del servidor", cfg->serverIp)) {
                                    strncpy(cfg->serverIp, ipbuf, sizeof(cfg->serverIp) - 1);
                                    cfg->serverIp[sizeof(cfg->serverIp) - 1] = 0;
                                    result = SL_DISCONNECT; goto leave;
                                }
                            } break;
                    case 8: if (act) state = ST_MENU; break;
                }
                if (kDown & KEY_B) state = ST_MENU;
            }
        }

        if (net_drain_video(&dec)) lastRx = osGetTime();
        video_update(&dec);

        u64 now = osGetTime();
        if (now - fpsT >= 1000) { videoFps = (int)(dec.framesPresented - lastFrames); lastFrames = dec.framesPresented; fpsT = now; }
        if (dec.droppedTiles != lastDropped) {
            lastDropped = dec.droppedTiles;
            if (now - lastKeyReq > 1000) { net_send_ctrl(CTRL_REQUEST_KEYFRAME, 0); lastKeyReq = now; }
        }
        if (now - lastRx > RX_TIMEOUT_MS) { result = SL_DISCONNECT; break; }

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(dynBuf);

        C2D_TargetClear(top, black);
        C2D_SceneBegin(top);
        video_draw(&dec, 0.0f, 0.0f);
        if (cfg->fpsHud) {
            char s[24]; snprintf(s, sizeof(s), "FPS: %d", videoFps);
            C2D_Text t; C2D_TextParse(&t, dynBuf, s); C2D_TextOptimize(&t);
            C2D_DrawRectSolid(2, 2, 0, 64, 18, fpsBg);
            C2D_DrawText(&t, C2D_WithColor, 6, 3, 0, 0.5f, 0.5f, fpsClr);
        }

        C2D_TargetClear(bottom, padBg);
        C2D_SceneBegin(bottom);
        if (state == ST_STREAM) {
            if (cfg->inputMode == INPUT_MODE_GAME) {
                C2D_DrawCircleSolid((float)VSTICK_CX, (float)VSTICK_CY, 0.0f, (float)VSTICK_R, padBase);
                C2D_DrawCircleSolid(kx, ky, 0.0f, (float)VSTICK_R * 0.42f, padKnob);
                C2D_DrawCircleSolid((float)VR3_CX, (float)VR3_CY, 0.0f, (float)VR3_R, r3 ? padKnob : padBase);
            } else {
                const bool izqDown = touched && in_rect(tp.px, tp.py, LBX, BTNY, LBW, BTNH);
                const bool derDown = touched && in_rect(tp.px, tp.py, RBX, BTNY, RBW, BTNH);
                C2D_DrawRectSolid(8, 26, 0, 304, 160, padBase);
                C2D_DrawText(&tTrack,     C2D_WithColor | C2D_AlignCenter, 160, 8,   0, 0.55f, 0.55f, white);
                C2D_DrawText(&tTrackHint, C2D_WithColor | C2D_AlignCenter, 160, 170, 0, 0.40f, 0.40f, dim);
                if (touched && (int)tp.py < 190) C2D_DrawCircleSolid((float)tp.px, (float)tp.py, 0.0f, 6.0f, padKnob);
                C2D_DrawRectSolid(LBX, BTNY, 0, LBW, BTNH, izqDown ? padSel : padKnob);
                C2D_DrawText(&tLClick, C2D_WithColor | C2D_AlignCenter, LBX + LBW/2, BTNY + 9, 0, 0.5f, 0.5f, white);
                C2D_DrawRectSolid(RBX, BTNY, 0, RBW, BTNH, derDown ? padSel : padKnob);
                C2D_DrawText(&tRClick, C2D_WithColor | C2D_AlignCenter, RBX + RBW/2, BTNY + 9, 0, 0.5f, 0.5f, white);
            }
        } else if (state == ST_MENU) {
            C2D_Text* lbl[3] = { &tKbd, &tConfig, &tExit };
            for (int r = 0; r < 3; ++r) {
                C2D_DrawRectSolid(MX, MY0 + r*MSTEP, 0, MW, MH, menuSel == r ? padSel : padBase);
                C2D_DrawText(lbl[r], C2D_WithColor | C2D_AlignCenter, MX + MW/2, MY0 + r*MSTEP + 11, 0, 0.7f, 0.7f, white);
            }
            C2D_DrawText(&tHint, C2D_WithColor | C2D_AlignCenter, 160, 222, 0, 0.42f, 0.42f, dim);
        } else { // ST_CONFIG
            for (int r = 0; r < CFG_ROWS; ++r)
                C2D_DrawRectSolid(RX, RY0 + r*RSTEP, 0, RW, RH, cfgSel == r ? padSel : padBase);
            const float cx = RX + RW/2;
            #define ROWY(r) (RY0 + (r)*RSTEP + 4)
            C2D_DrawText(cfg->inputMode == INPUT_MODE_GAME ? &tModeGame : &tModeDesk, C2D_WithColor | C2D_AlignCenter, cx, ROWY(0), 0, 0.45f, 0.45f, white);
            C2D_DrawText(cfg->buttonSwap ? &tBtnPos : &tBtnName, C2D_WithColor | C2D_AlignCenter, cx, ROWY(1), 0, 0.45f, 0.45f, white);
            C2D_DrawText(cfg->fpsHud ? &tHudOn : &tHudOff, C2D_WithColor | C2D_AlignCenter, cx, ROWY(2), 0, 0.45f, 0.45f, white);
            char b3[24], b4[24], b5[24], b6[24], b7[80];
            snprintf(b3, sizeof(b3), "Calidad: %d", cfg->quality);
            snprintf(b4, sizeof(b4), "FPS: %d", cfg->fps);
            snprintf(b5, sizeof(b5), "Sensib: %d%%", cfg->sensPercent);
            snprintf(b6, sizeof(b6), "Z.muerta: %d", cfg->deadzone);
            snprintf(b7, sizeof(b7), "IP: %s", cfg->serverIp);
            const char* dyn[5] = { b3, b4, b5, b6, b7 };
            for (int i = 0; i < 5; ++i) {
                C2D_Text t; C2D_TextParse(&t, dynBuf, dyn[i]); C2D_TextOptimize(&t);
                C2D_DrawText(&t, C2D_WithColor | C2D_AlignCenter, cx, ROWY(3 + i), 0, 0.45f, 0.45f, white);
            }
            C2D_DrawText(&tBack, C2D_WithColor | C2D_AlignCenter, cx, ROWY(8), 0, 0.45f, 0.45f, white);
            #undef ROWY
        }
        C3D_FrameEnd(0);
    }

leave:
    video_exit(&dec);
    return result;
}

int main(void) {
    gfxInitDefault();
    AppConfig cfg;
    appconfig_load(&cfg);
    if (!net_init()) { gfxExit(); return 1; }

    Ui ui;
    ui_init(&ui);

    bool quit = false;
    while (!quit && aptMainLoop()) {
        ServerHello sh;
        int  fails = 0;
        bool connected = false;
        while (aptMainLoop()) {
            draw_connecting(&ui, cfg.serverIp);
            if (net_handshake(cfg.serverIp, &sh)) { connected = true; break; }
            if (++fails >= 2) {
                char ipbuf[64];
                if (swkbd_get(ipbuf, sizeof(ipbuf), "No conecta. IP del servidor:", cfg.serverIp)) {
                    strncpy(cfg.serverIp, ipbuf, sizeof(cfg.serverIp) - 1);
                    cfg.serverIp[sizeof(cfg.serverIp) - 1] = 0;
                    appconfig_save(&cfg);
                }
                fails = 0;
            } else {
                svcSleepThread(1000000000ULL);   // 1s
            }
        }
        if (!connected) break;
        if (sh.codec != CODEC_RAW_YUV420 && sh.codec != CODEC_JPEG_YCBCR) break;

        if (net_open_streams(cfg.serverIp, &sh)) {
            int r = stream_loop(&ui, &cfg, &sh);
            if (r == SL_QUIT) quit = true;
        }
        appconfig_save(&cfg);
        net_close_streams();
    }

    ui_exit(&ui);
    net_exit();
    gfxExit();
    return 0;
}
