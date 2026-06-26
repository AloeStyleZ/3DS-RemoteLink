// Cliente de streaming 3DS.
//   1) Handshake TCP con el PC (negocia resolucion/codec/puertos).
//   2) Cada frame: captura input (libctru) -> envia por UDP -> drena video ->
//      reensambla -> Y2R -> dibuja con citro2d.
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "protocol.h"
#include "config.h"
#include "net.h"
#include "decoder.h"

// Bucle de espera final mostrando logs en la pantalla inferior.
static void wait_for_exit(void) {
    printf("\nPulsa START para salir.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

static void stream_loop(const ServerHello* sh) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    C3D_RenderTarget* top    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    const u32 black   = C2D_Color32(0, 0, 0, 0xFF);
    const u32 padBg   = C2D_Color32(0x1c, 0x1c, 0x24, 0xFF);
    const u32 padBase = C2D_Color32(0x3a, 0x40, 0x52, 0xFF);
    const u32 padKnob = C2D_Color32(0x7a, 0xc0, 0xff, 0xFF);

    VideoDecoder dec;
    if (!video_init(&dec, sh->width, sh->height, sh->tile_w, sh->tile_h)) {
        printf("video_init fallo\n");
        C2D_Fini(); C3D_Fini();
        return;
    }

    printf("Streaming. START+SELECT para salir.\n");
    u16  inputSeq  = 0;
    bool prevTouch = false;
    int  engaged   = 0;   // zona tactil enganchada: 0=nada, 1=stick, 2=R3

    while (aptMainLoop()) {
        hidScanInput();
        const u32 held = hidKeysHeld();
        if ((held & KEY_START) && (held & KEY_SELECT)) break;

        // 1) Input -> PC PRIMERO (minimiza latencia extremo a extremo).
        circlePosition cp; hidCircleRead(&cp);
        touchPosition  tp; hidTouchRead(&tp);
        const bool touched = (held & KEY_TOUCH) != 0;

        // Al tocar se "engancha" la zona (stick o R3) y se mantiene hasta soltar,
        // aunque el dedo se salga del area -> evita recentrados por deslices.
        if (touched && !prevTouch) {
            int sdx = (int)tp.px - VSTICK_CX, sdy = (int)tp.py - VSTICK_CY;
            int bdx = (int)tp.px - VR3_CX,    bdy = (int)tp.py - VR3_CY;
            if (sdx * sdx + sdy * sdy <= VSTICK_R * VSTICK_R)  engaged = 1; // stick
            else if (bdx * bdx + bdy * bdy <= VR3_R * VR3_R)   engaged = 2; // R3
            else                                              engaged = 0;
        }
        if (!touched) engaged = 0;
        prevTouch = touched;

        s16   rx = 0, ry = 0;
        bool  r3 = false;
        float kx = (float)VSTICK_CX, ky = (float)VSTICK_CY;
        if (engaged == 1) {                              // stick derecho
            float dx = (float)((int)tp.px - VSTICK_CX);
            float dy = (float)((int)tp.py - VSTICK_CY);
            float d2 = dx * dx + dy * dy;
            if (d2 > (float)(VSTICK_R * VSTICK_R)) {     // fuera del circulo -> fijar al borde
                float m = (float)VSTICK_R / sqrtf(d2);
                dx *= m; dy *= m;
            }
            rx = (s16)(dx * 32767.0f / VSTICK_R);
            ry = (s16)(-dy * 32767.0f / VSTICK_R);       // pantalla Y abajo -> invertir
            kx = VSTICK_CX + dx; ky = VSTICK_CY + dy;
        } else if (engaged == 2) {                       // boton R3
            r3 = true;
        }

        InputPacket ip;
        ip.magic    = PROTO_MAGIC;
        ip.type     = PKT_INPUT;
        ip.seq      = inputSeq++;
        ip.buttons  = held;          // hidKeysHeld() == bitfield BTN_* del protocolo
        ip.circle_x = cp.dx;
        ip.circle_y = cp.dy;
        ip.right_x  = rx;
        ip.right_y  = ry;
        ip.vbuttons = r3 ? VBTN_R3 : 0;
        ip._pad     = 0;
        net_send_input(&ip);

        // 2) Drenar video entrante (reensambla; solo marca frame completo).
        net_drain_video(&dec);
        // Un unico Y2R+present por iteracion, sobre el frame mas reciente.
        video_update(&dec);

        // 3) Render: video arriba, controles tactiles abajo.
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, black);
        C2D_SceneBegin(top);
        video_draw(&dec, 0.0f, 0.0f);

        C2D_TargetClear(bottom, padBg);
        C2D_SceneBegin(bottom);
        // stick derecho (base + knob)
        C2D_DrawCircleSolid((float)VSTICK_CX, (float)VSTICK_CY, 0.0f, (float)VSTICK_R, padBase);
        C2D_DrawCircleSolid(kx, ky, 0.0f, (float)VSTICK_R * 0.42f, padKnob);
        // boton R3 (se ilumina al pulsar)
        C2D_DrawCircleSolid((float)VR3_CX, (float)VR3_CY, 0.0f, (float)VR3_R, r3 ? padKnob : padBase);
        C3D_FrameEnd(0);
    }

    video_exit(&dec);
    C2D_Fini();
    C3D_Fini();
}

int main(void) {
    gfxInitDefault();
    // NOTA: no usamos consoleInit. citro2d controla ambas pantallas (video arriba,
    // stick abajo), y consoleInit dejaba la pantalla inferior en un formato que
    // descuadraba el render de citro2d. Los logs de arranque/handshake se ven en
    // la consola del servidor (PC). Los printf de abajo quedan como no-op inofensivo.

    printf("3DSStreaming client\nServidor: %s\n", SERVER_IP);

    if (net_init()) {
        ServerHello sh;
        printf("Handshake con %s:%d ...\n", SERVER_IP, PORT_HANDSHAKE);
        if (net_handshake(SERVER_IP, &sh)) {
            printf("OK: %ux%u codec=%u tiles=%ux%u fps=%u\n",
                   sh.width, sh.height, sh.codec, sh.tiles_x, sh.tiles_y, sh.fps);
            if (sh.codec != CODEC_RAW_YUV420 && sh.codec != CODEC_JPEG_YCBCR) {
                printf("Codec no soportado: %u\n", sh.codec);
            } else if (net_open_streams(SERVER_IP, &sh)) {
                stream_loop(&sh);
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

    wait_for_exit();
    gfxExit();
    return 0;
}
